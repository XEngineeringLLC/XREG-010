#!/usr/bin/env python3
# Offline tests for the GX app updater (bridge/xreg_update.py) and its D-Bus paths on the Manager: the RSA check against
# openssl and the firmware's key, the release list with builds, the download and staging, the downgrade and signature
# refusals, what install.sh reports back and how its lock is read, and the detached install end to end on a fake GX
# (gx_app/tools/sim_install.py): a bridge process killed by the restart while install.sh carries on, a good update, a
# rollback, and a version issued again. Run: python3.12 -B test_update.py
import base64, hashlib, json, os, re, shutil, subprocess, sys, tempfile, threading, time, unittest, zipfile
import urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GXAPP = REPO + '/gx_app'
BRIDGE = os.environ.get('XREG_BRIDGE', GXAPP + '/xregulator/bridge')
sys.path.insert(0, BRIDGE)
sys.path.insert(0, GXAPP + '/tools')
sys.path.insert(0, GXAPP)
import xreg_update as xu
import sim_install as sim
import build_package as bp

KEYS = tempfile.mkdtemp(prefix='xreg_keys_')
PKG = os.path.join(KEYS, 'venus-data.zip')
B = 1761800000   # the test package's build
R03 = '0.3+%d' % B


def openssl(*args, **kw):
    return subprocess.run(['openssl'] + list(args), check=True, capture_output=True, **kw)


def setUpModule():
    for name in ('test', 'other'):
        openssl('genrsa', '-out', os.path.join(KEYS, name + '.pem'), '4096')
        openssl('rsa', '-in', os.path.join(KEYS, name + '.pem'), '-pubout', '-out', os.path.join(KEYS, name + '.pub'))
    bp.build_zip(PKG, '0.3', B)
    bp.check_zip(PKG, R03)


def tearDownModule():
    shutil.rmtree(KEYS, ignore_errors=True)


def pub(name='test'):
    with open(os.path.join(KEYS, name + '.pub')) as f:
        return xu.load_public_key(f.read())


def sign(path, name='test'):
    out = os.path.join(KEYS, 'sig.bin')
    openssl('dgst', '-sha256', '-sign', os.path.join(KEYS, name + '.pem'), '-out', out, path)
    with open(out, 'rb') as f:
        return f.read()


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).digest()


class Server:
    # the OTA host: name -> bytes (a list of chunks is sent one per 0.3 s); every request kept, as the proxy sees it
    def __init__(self, files):
        self.files, self.seen = files, []
        outer = self

        class H(BaseHTTPRequestHandler):
            def do_GET(self):
                outer.seen.append((self.path, sorted(self.headers.keys()), self.headers.get('User-Agent', '')))
                body = outer.files.get(self.path.lstrip('/').split('?')[0])
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                chunks = body if isinstance(body, list) else [body]
                self.send_response(200)
                self.send_header('Content-Length', str(sum(len(c) for c in chunks)))
                self.end_headers()
                try:
                    for c in chunks:
                        self.wfile.write(c)
                        self.wfile.flush()
                        if len(chunks) > 1:
                            time.sleep(0.3)
                except OSError:
                    pass

            def log_message(self, *a):
                pass
        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.base = 'http://127.0.0.1:%d/' % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def release(files, pkg, signer='test', sha=True, listed=None, build=None):
    # the package, its .sig and its release-list entry, laid out as build_package.py and publish_gx.sh do
    with open(pkg, 'rb') as f:
        data = f.read()
    with zipfile.ZipFile(pkg) as z:
        v, b = xu.parse_release(xu.package_release(z))
    v, b = listed or v, b if build is None else build
    name = 'gxapp_%s_%s' % (v, hashlib.sha256(data).hexdigest()[:12])
    files[name + '.zip'] = data
    files[name + '.sig'] = base64.b64encode(sign(pkg, signer))
    m = json.loads(files.get('gxapp_versions.json', b'{"versions": {}}'))
    m['versions'][v] = {'build': b, 'file': name + '.zip', 'sig': name + '.sig', 'size': len(data),
                        'sha256': hashlib.sha256(data).hexdigest() if sha else ''}
    files['gxapp_versions.json'] = json.dumps(m).encode()
    return name


def wait(pred, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


class TestSignature(unittest.TestCase):
    def test_good_and_bad(self):
        k = pub()
        sig = sign(PKG)
        self.assertTrue(xu.verify_digest(digest(PKG), sig, k))
        with open(PKG, 'rb') as f:
            bad = bytearray(f.read())
        bad[100] ^= 1
        self.assertFalse(xu.verify_digest(hashlib.sha256(bad).digest(), sig, k))      # one bit of the package
        s2 = bytearray(sig)
        s2[-1] ^= 1
        self.assertFalse(xu.verify_digest(digest(PKG), bytes(s2), k))                 # one bit of the signature
        self.assertFalse(xu.verify_digest(digest(PKG), sig[1:], k))                   # short
        self.assertFalse(xu.verify_digest(digest(PKG), sig, pub('other')))            # another key
        self.assertFalse(xu.verify_digest(digest(PKG), sign(PKG, 'other'), k))        # signed by another key
        self.assertFalse(xu.verify_digest(digest(PKG), (k[0] + 1).to_bytes(512, 'big'), k))   # not below n
        self.assertFalse(xu.verify_digest(digest(PKG), b'\x00' * 512, k))

    def test_matches_openssl_verify(self):
        k = pub()
        good, other = sign(PKG), sign(PKG, 'other')
        for s in (good, other, good[:-1] + bytes([good[-1] ^ 0x80])):
            p = os.path.join(KEYS, 'v.sig')
            with open(p, 'wb') as f:
                f.write(s)
            ossl = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', os.path.join(KEYS, 'test.pub'),
                                   '-signature', p, PKG], capture_output=True).returncode == 0
            self.assertEqual(xu.verify_digest(digest(PKG), s, k), ossl)

    def test_embedded_key_is_the_firmware_ota_key(self):
        with open(REPO + '/Xregulator.ino') as f:
            src = f.read()
        lit = re.search(r'OTA_PUBLIC_KEY\s*=\s*((?:\s*"[^"]*")+)\s*;', src).group(1)
        pem = ''.join(re.findall(r'"([^"]*)"', lit)).replace('\\n', '\n')
        self.assertEqual(xu.load_public_key(pem), xu.load_public_key(xu.OTA_PUBLIC_KEY))
        n, e = xu.load_public_key(xu.OTA_PUBLIC_KEY)
        p = os.path.join(KEYS, 'ota.pub')
        with open(p, 'w') as f:
            f.write(xu.OTA_PUBLIC_KEY)
        mod = openssl('rsa', '-pubin', '-in', p, '-noout', '-modulus', text=True).stdout.strip().split('=')[1]
        self.assertEqual((n, e, n.bit_length()), (int(mod, 16), 65537, 4096))


class TestReleases(unittest.TestCase):
    def test_order(self):
        self.assertTrue(xu.newer('0.10', '0.9') and xu.newer('0.3.1', '0.3') and not xu.newer('0.3', '0.3.0'))
        self.assertTrue(xu.newer('0.3+2', '0.3+1') and not xu.newer('0.3+1', '0.3+2') and not xu.newer('0.3+1', '0.3+1'))
        self.assertTrue(xu.newer('0.4+0', '0.3+9999999999') and xu.newer('0.3+1', '0.3'))
        self.assertTrue(xu.newer('0.3', '0') and not xu.newer('x', '0.1') and xu.newer('0.1', 'bad'))

    def test_strict_patterns(self):
        for bad in ('0.3\n', '0.٣', '٠.3', '0.3 ', '0..3', '', '0.3+', '0.3+1\n', '0.3+-1', '0.3+١',
                    '0.3+007', '0.3+00', '0.3+1000000000000'):
            self.assertIsNone(xu.parse_release(bad), repr(bad))
        self.assertIsNone(xu.SHA_RE.match('a' * 64 + '\n'))
        self.assertIsNone(xu.NAME_RE.match('gxapp.zip\n'))
        self.assertEqual(xu.parse_release('0.3+1761800000'), ('0.3', 1761800000))

    def test_manifest(self):
        m = {'versions': {v: {'build': b, 'file': 'gxapp_%s.zip' % v, 'sig': 'gxapp_%s.sig' % v, 'size': 10}
                          for v, b in (('0.9', 5), ('0.10', 1), ('0.3', 9))}}
        self.assertEqual(xu.parse_manifest(json.dumps(m).encode())[0], '0.10+1')
        for body in (b'not json', b'[]', b'{"versions": []}', b'{}', b'\xff\xfe'):
            with self.assertRaises(xu.UpdateError):
                xu.parse_manifest(body)
        bad = {'versions': {'0.4': {'file': '../etc/passwd', 'sig': 'a.sig'}, '0.5': {'file': 'gx/app.zip', 'sig': 'a.sig'},
                            '0.6': {'file': 'a.zip', 'sig': 'a.sig', 'size': xu.PACKAGE_MAX + 1},
                            'v0.7': {'file': 'a.zip', 'sig': 'b'}, '0.8': 'x', '0.9': {'file': 'a.zip'},
                            '0.11': {'file': 'a.zip', 'sig': 'a.sig', 'build': -1},
                            '0.12': {'file': 'a.zip', 'sig': 'a.sig', 'build': '7'},
                            '0.13\n': {'file': 'a.zip', 'sig': 'a.sig'}}}
        with self.assertRaises(xu.UpdateError):
            xu.parse_manifest(json.dumps(bad).encode())
        bad['versions']['0.2'] = {'file': 'gxapp_0.2.zip', 'sig': 'gxapp_0.2.sig', 'sha256': 'nothex'}
        latest, e = xu.parse_manifest(json.dumps(bad).encode())
        self.assertEqual((latest, e['sha256'], e['size'], e['build']), ('0.2+0', '', 0, 0))   # every listed fault skipped

    def test_result_formats(self):
        t = tempfile.mkdtemp()
        try:
            for line, want in (
                    ('ok 0.4+7 0.3+5 1761780000 0a1b2c3d note here\n',
                     {'result': 'ok', 'to': '0.4', 'toBuild': 7, 'from': '0.3', 'fromBuild': 5, 'at': 1761780000,
                      'id': '0a1b2c3d', 'note': 'note here'}),
                    ('rolledback 0.4+7 - 1761780000 - \n',
                     {'result': 'rolledback', 'to': '0.4', 'toBuild': 7, 'from': '', 'fromBuild': 0, 'at': 1761780000,
                      'id': '-', 'note': ''}),
                    ('ok 0.3 0.2 1761791700 \n',   # what 0.3's install.sh wrote on the bench
                     {'result': 'ok', 'to': '0.3', 'toBuild': 0, 'from': '0.2', 'fromBuild': 0, 'at': 1761791700,
                      'id': '-', 'note': ''}),
                    ('rolledback 0.4 0.3 1761791700 Version 0.4 did not start.\n',
                     {'result': 'rolledback', 'to': '0.4', 'toBuild': 0, 'from': '0.3', 'fromBuild': 0,
                      'at': 1761791700, 'id': '-', 'note': 'Version 0.4 did not start.'})):
                with open(os.path.join(t, 'result'), 'w') as f:
                    f.write(line)
                self.assertEqual(xu.read_result(t), want)
            for line in ('garbage\n', 'ok 0.4 - notanumber\n', 'ok 0.4 - ١٢ x\n'):
                with open(os.path.join(t, 'result'), 'w') as f:
                    f.write(line)
                self.assertIsNone(xu.read_result(t))
        finally:
            shutil.rmtree(t)


class UpdaterCase(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp(prefix='xreg_upd_')
        self.app, self.work = os.path.join(self.t, 'app'), os.path.join(self.t, 'work')
        xu.stage(PKG, self.app)
        self.files = {}
        self.srv = Server(self.files)
        self.spawned = []
        self.u = xu.Updater('0.3', self.app, self.work, base=self.srv.base, key=pub(),
                            spawn=lambda args, work, run_id: self.spawned.append((args, run_id)))

    def tearDown(self):
        self.srv.close()
        shutil.rmtree(self.t, ignore_errors=True)

    def settle(self):
        self.assertTrue(wait(lambda: self.u.state not in (xu.CHECKING, xu.DOWNLOADING, xu.VERIFYING)), self.u.state)
        self.u.next_poll = 0
        return self.u.values()

    def bumped(self, v, build=B, listed=None, **kw):
        p = sim.bump(PKG, v, os.path.join(self.t, 'p%s_%d.zip' % (v, build)), build=build)
        release(self.files, p, listed=listed, **kw)
        return p


class TestUpdater(UpdaterCase):
    def test_check_current_and_available(self):
        release(self.files, PKG)
        self.assertEqual((self.u.rid, self.u.values()['/Update/InstalledBuild']), (R03, B))
        self.assertTrue(self.u.on_write('/Update/Check', 1))
        v = self.settle()
        self.assertEqual((v['/Update/State'], v['/Update/Latest'], v['/Update/LatestBuild'], v['/Update/Installed']),
                         (xu.CURRENT, '0.3', B, '0.3'))
        self.bumped('0.4')
        self.u.check()
        v = self.settle()
        self.assertEqual((v['/Update/State'], v['/Update/Latest'], v['/Update/LatestBuild']), (xu.AVAILABLE, '0.4', B))
        self.assertGreater(v['/Update/Size'], 100000)
        # plain GETs for the release list, a throwaway query, nothing about this GX in the path or the headers
        self.assertEqual(len(self.srv.seen), 2)
        queries = []
        for path, headers, ua in self.srv.seen:
            self.assertRegex(path, r'^/gxapp_versions\.json\?r=[0-9a-f]{8}$')
            queries.append(path)
            self.assertLessEqual(set(headers), {'Host', 'User-Agent', 'Accept-Encoding', 'Connection'})
            self.assertTrue(ua.startswith('Python-urllib/'), ua)
        self.assertNotEqual(queries[0], queries[1])

    def test_unreadable_build_offers_the_listed_release(self):
        # a BUILD install.sh would refuse (here a leading zero) is no release to compare: the listed one is offered
        with open(os.path.join(self.app, 'BUILD'), 'w') as f:
            f.write('0%d\n' % B)
        u = xu.Updater('0.3', self.app, self.work, base=self.srv.base, key=pub(), spawn=lambda *a: None)
        self.assertEqual((u.rid, u.values()['/Update/InstalledBuild']), ('', 0))
        release(self.files, PKG)
        u.check()
        self.assertTrue(wait(lambda: u.state != xu.CHECKING))
        self.assertEqual((u.state, u.latest), (xu.AVAILABLE, R03))

    def test_reissued_version(self):
        # the same version number with a newer build is offered and installs; an older build is not
        self.bumped('0.3', build=B - 50)
        self.u.check()
        self.assertEqual(self.settle()['/Update/State'], xu.CURRENT)
        self.assertFalse(self.u.install('0.3+%d' % (B - 50)))
        self.files.clear()
        self.bumped('0.3', build=B + 50)
        self.u.check()
        v = self.settle()
        self.assertEqual((v['/Update/State'], v['/Update/Latest'], v['/Update/LatestBuild']), (xu.AVAILABLE, '0.3', B + 50))
        self.assertFalse(self.u.on_write('/Update/Install', '0.3'))     # the release as the page showed it, build too
        self.assertTrue(self.u.on_write('/Update/Install', '0.3+%d' % (B + 50)))
        self.assertTrue(wait(lambda: self.spawned))
        self.assertEqual(open(os.path.join(self.work, 'new', 'BUILD')).read().strip(), str(B + 50))
        # the file names carry the content hash: a version issued again never reuses a name
        self.assertRegex(self.srv.seen[-2][0], r'^/gxapp_0\.3_[0-9a-f]{12}\.zip$')

    def test_check_errors(self):
        self.u.check()
        v = self.settle()
        self.assertEqual((v['/Update/State'], v['/Update/Message']), (xu.FAILED, 'ota.xengineering.net answered HTTP 404.'))
        self.assertEqual(v['/Update/Last'], '')   # a failed check is not an install result
        self.files['gxapp_versions.json'] = b'<html>'
        self.u.check()
        self.assertEqual(self.settle()['/Update/Message'], 'The release list from ota.xengineering.net is not readable.')
        self.srv.close()
        self.u.check()
        v = self.settle()
        self.assertEqual(v['/Update/State'], xu.FAILED)
        self.assertTrue(v['/Update/Message'].startswith('No answer from ota.xengineering.net ('), v['/Update/Message'])
        self.srv = Server(self.files)   # for tearDown

    def test_install_stages_and_spawns(self):
        self.bumped('0.4')
        self.u.check()
        self.settle()
        self.assertFalse(self.u.on_write('/Update/Install', '0.5+%d' % B))   # only the newest listed
        self.assertTrue(self.u.on_write('/Update/Install', '0.4+%d' % B))
        self.assertFalse(self.u.on_write('/Update/Check', 1))                # busy
        self.assertTrue(wait(lambda: self.spawned))
        new = os.path.join(self.work, 'new')
        (args, run_id), = self.spawned
        self.assertEqual(args, ['sh', new + '/install.sh', 'install', new, '--upgrade-only'])
        self.assertRegex(run_id, r'^[0-9a-f]{8}$')
        self.assertEqual(self.u.values()['/Update/State'], xu.INSTALLING)
        self.assertEqual(open(new + '/VERSION').read().strip(), '0.4')
        self.assertTrue(os.access(new + '/install.sh', os.X_OK) and os.access(new + '/service/run', os.X_OK))
        self.assertFalse(os.access(new + '/bridge/xreg_core.py', os.X_OK))
        self.assertFalse(os.path.exists(os.path.join(self.work, 'download')))

    def test_refusals_stay_on_the_page(self):
        self.bumped('0.4', signer='other')
        self.u.check()
        self.settle()
        self.u.install('0.4+%d' % B)
        v = self.settle()
        msg = 'The package signature does not match. Nothing was installed.'
        self.assertEqual((v['/Update/State'], v['/Update/Message']), (xu.FAILED, msg))
        last = json.loads(v['/Update/Last'])
        self.assertEqual((last['result'], last['to'], last['from'], last['note']), ('failed', '0.4', '0.3', msg))
        self.assertEqual(self.spawned, [])
        self.assertFalse(os.path.exists(os.path.join(self.work, 'new')))
        # a check afterwards replaces the check state, never the install result
        self.u.check()
        v = self.settle()
        self.assertEqual((v['/Update/State'], v['/Update/Message']), (xu.AVAILABLE, ''))
        self.assertEqual(json.loads(v['/Update/Last'])['note'], msg)
        name = [n for n in self.files if n.endswith('.sig')][0]
        self.files[name] = b'@@not base64@@'
        self.u.install('0.4+%d' % B)
        self.assertEqual(self.settle()['/Update/Message'], msg)
        zname = [n for n in self.files if n.endswith('.zip')][0]
        self.files[zname] = self.files[zname][:-10] + b'0123456789'
        self.u.install('0.4+%d' % B)
        self.assertEqual(self.settle()['/Update/Message'], 'The download is damaged. Nothing was installed.')

    def test_unlisted_sha_still_needs_the_signature(self):
        self.bumped('0.4', sha=False)
        zname = [n for n in self.files if n.endswith('.zip')][0]
        z = bytearray(self.files[zname])
        z[-30] ^= 1
        self.files[zname] = bytes(z)
        self.u.check()
        self.settle()
        self.u.install('0.4+%d' % B)
        self.assertEqual(self.settle()['/Update/Message'], 'The package signature does not match. Nothing was installed.')
        self.assertEqual(self.spawned, [])

    def test_downgrade_refused(self):
        release(self.files, sim.bump(PKG, '0.2', os.path.join(self.t, 'old.zip')))
        self.u.check()
        self.assertEqual(self.settle()['/Update/State'], xu.CURRENT)
        self.assertFalse(self.u.install('0.2+%d' % B))
        # an older, validly signed package listed as a newer version: its own VERSION decides
        self.files.clear()
        release(self.files, sim.bump(PKG, '0.2', os.path.join(self.t, 'old.zip')), listed='0.9')
        self.u.check()
        self.assertEqual(self.settle()['/Update/State'], xu.AVAILABLE)
        self.u.install('0.9+%d' % B)
        v = self.settle()
        self.assertEqual(v['/Update/Message'],
                         'The package is release 0.2+%d, not 0.9+%d. Nothing was installed.' % (B, B))
        # the installed release listed again with a newer build than it has: the package's own BUILD decides
        self.files.clear()
        release(self.files, PKG, build=B + 9)
        self.u.check()
        self.settle()
        self.u.install('0.3+%d' % (B + 9))
        self.assertEqual(self.settle()['/Update/Message'],
                         'The package is release %s, not 0.3+%d. Nothing was installed.' % (R03, B + 9))
        self.assertEqual(self.spawned, [])

    def test_limits(self):
        self.bumped('0.4')
        self.u.check()
        self.settle()
        old = xu.PACKAGE_MAX
        xu.PACKAGE_MAX = 1000
        try:
            self.u.install('0.4+%d' % B)
            self.assertTrue(self.settle()['/Update/Message'].endswith('.zip is larger than expected. Nothing was installed.'))
        finally:
            xu.PACKAGE_MAX = old
        # a download that trickles past its overall deadline
        zname = [n for n in self.files if n.endswith('.zip')][0]
        data = self.files[zname]
        self.files[zname] = [data[i:i + 4096] for i in range(0, len(data), 4096)]
        old = xu.DOWNLOAD_S
        xu.DOWNLOAD_S = 1.0
        try:
            self.u.install('0.4+%d' % B)
            self.assertEqual(self.settle()['/Update/Message'], 'The download took longer than 1 s. Nothing was installed.')
        finally:
            xu.DOWNLOAD_S = old
        self.assertEqual(self.spawned, [])

    def test_bad_entries(self):
        p4 = self.bumped('0.4')
        evil = os.path.join(self.t, 'evil.zip')
        with zipfile.ZipFile(p4) as z, zipfile.ZipFile(evil, 'w') as o:
            for zi in z.infolist():
                o.writestr(zi, z.read(zi))
            o.writestr(xu.PAYLOAD + '../../../rc.local', b'boom')
        self.files.clear()
        release(self.files, evil)
        self.u.check()
        self.settle()
        self.u.install('0.4+%d' % B)
        self.assertTrue(self.settle()['/Update/Message'].startswith('The package holds an unexpected entry'))
        self.assertEqual(self.spawned, [])

    def test_no_proxy_from_the_environment(self):
        os.environ['http_proxy'] = 'http://127.0.0.1:9'   # nothing listens there
        try:
            release(self.files, PKG)
            with self.assertRaises(urllib.error.URLError):   # urllib's default opener would take it
                urllib.request.build_opener().open(self.srv.base + 'gxapp_versions.json', timeout=3)
            u = xu.Updater('0.3', self.app, self.work, base=self.srv.base, key=pub(), spawn=lambda *a: None)
            u.check()
            self.assertTrue(wait(lambda: u.state != xu.CHECKING))
            self.assertEqual(u.state, xu.CURRENT, u.message)
        finally:
            del os.environ['http_proxy']

    def test_remove_spawns_a_private_copy(self):
        self.assertFalse(self.u.on_write('/Update/Remove', 0))
        self.assertTrue(self.u.on_write('/Update/Remove', 1))
        self.assertTrue(wait(lambda: self.spawned))
        (args, run_id), = self.spawned
        self.assertEqual((args[0], args[2], run_id), ('sh', 'remove', '-'))
        self.assertTrue(os.path.basename(args[1]).startswith('xregulator-remove-'))
        self.assertNotEqual(os.path.basename(args[1]), 'xregulator-remove.sh')
        self.assertEqual(os.stat(args[1]).st_mode & 0o077, 0)   # mkstemp: the owner's alone
        with open(args[1], 'rb') as a, open(os.path.join(self.app, 'install.sh'), 'rb') as b:
            self.assertEqual(a.read(), b.read())
        os.remove(args[1])
        self.assertEqual(self.u.values()['/Update/State'], xu.REMOVING)
        self.assertFalse(self.u.check())


class TestInstallReports(UpdaterCase):
    def write(self, name, text):
        os.makedirs(os.path.dirname(os.path.join(self.work, name)), exist_ok=True)
        with open(os.path.join(self.work, name), 'w') as f:
            f.write(text)

    def fresh(self):
        self.u.next_poll = 0
        return self.u.values()

    def hold_lock(self, pid=None, boot=None):
        self.write('lock/owner', '%d %s\n' % (pid or os.getpid(), xu.boot_id() if boot is None else boot))

    def test_installing_only_while_install_sh_holds_its_lock(self):
        self.write('pending', '0.4+7 %s %s -\n' % (R03, xu.boot_id()))
        self.assertEqual(self.fresh()['/Update/State'], xu.IDLE)        # no lock: nothing is running
        os.makedirs(os.path.join(self.work, 'lock'))
        self.assertEqual(self.fresh()['/Update/State'], xu.IDLE)        # a lock dir with no owner
        dead = subprocess.Popen(['true'])
        dead.wait()
        self.hold_lock(pid=dead.pid)
        self.assertEqual(self.fresh()['/Update/State'], xu.IDLE)        # its process is gone
        self.hold_lock(boot='an-earlier-boot')
        self.assertEqual(self.fresh()['/Update/State'], xu.IDLE)        # another boot's
        self.hold_lock()
        self.assertEqual(self.fresh()['/Update/State'], xu.INSTALLING)  # live
        shutil.rmtree(os.path.join(self.work, 'lock'))
        self.write('confirming', '%d %s\n' % (os.getpid(), xu.boot_id()))
        self.assertEqual(self.fresh()['/Update/State'], xu.INSTALLING)  # a confirm waiting without the lock
        os.remove(os.path.join(self.work, 'confirming'))
        self.assertEqual(self.fresh()['/Update/State'], xu.IDLE)
        self.hold_lock()
        self.assertEqual(self.fresh()['/Update/State'], xu.INSTALLING)
        self.assertFalse(self.u.check())
        shutil.rmtree(os.path.join(self.work, 'lock'))
        os.remove(os.path.join(self.work, 'pending'))
        self.write('result', 'ok 0.4+7 %s 1761780000 - \n' % R03)
        v = self.fresh()
        self.assertEqual(v['/Update/State'], xu.IDLE)
        self.assertEqual(json.loads(v['/Update/Last']), {'result': 'ok', 'to': '0.4', 'toBuild': 7, 'from': '0.3',
                                                         'fromBuild': B, 'at': 1761780000, 'note': ''})

    def test_refused_by_install_sh_after_a_wall_clock_jump(self):
        # this bridge started install.sh, which refused the package and never restarted it; the wall clock in the
        # result is a day off, which must not matter: the run id ties the result to the install
        self.u.state, self.u.run_id, self.u.spawned = xu.INSTALLING, 'abcd1234', self.u.clock()
        self.write('result', 'failed 0.4+7 %s %d abcd1234 It is not newer than the installed version.\n'
                   % (R03, time.time() - 86400))
        v = self.fresh()
        self.assertEqual((v['/Update/State'], v['/Update/Message']),
                         (xu.FAILED, 'It is not newer than the installed version.'))
        # another install's result does not end this one
        self.u.state, self.u.run_id, self.u.spawned = xu.INSTALLING, 'abcd1234', self.u.clock()
        self.write('result', 'ok 0.4+7 %s %d - \n' % (R03, time.time()))
        self.assertEqual(self.fresh()['/Update/State'], xu.INSTALLING)
        self.u.spawned -= xu.SPAWN_STALE_S + 1
        v = self.fresh()
        self.assertEqual((v['/Update/State'], v['/Update/Message']), (xu.FAILED, 'The installer stopped without a result.'))

    def test_running_marker(self):
        os.environ['XREG_BOOT_ID'] = 'b1'
        try:
            self.assertFalse(xu.mark_running(self.work, R03))
        finally:
            del os.environ['XREG_BOOT_ID']
        with open(os.path.join(self.work, 'running')) as f:
            self.assertEqual(f.read(), '%s %d b1\n' % (R03, os.getpid()))
        xu.mark_running('/nonexistent/x', '0.3')   # logged, never raised


class TestManagerPaths(UpdaterCase):
    def test_dbus_paths(self):
        sys.path.insert(0, HERE)
        import test_bridge as tb
        xc = tb.xc
        release(self.files, PKG)
        mgr = xc.Manager(tb.Store(), tb.SCHEMA_TEXT, xc.Discovery(browse=False, sweep_hosts=lambda: []),
                         lambda uid, inst: tb.Store(), updater=self.u)
        p = tb.Pub(mgr)
        for path in ('/Update/Installed', '/Update/InstalledBuild', '/Update/Latest', '/Update/LatestBuild',
                     '/Update/State', '/Update/Progress', '/Update/Size', '/Update/Message', '/Update/Last',
                     '/Update/Check', '/Update/Install', '/Update/Remove'):
            self.assertIn(path, p)
        self.assertEqual({k for k, m in p.modes.items() if k.startswith('/Update/') and m == 'core'},
                         {'/Update/Check', '/Update/Install', '/Update/Remove'})
        mgr.start()
        try:
            self.assertTrue(mgr.on_write('/Update/Check', 1))
            self.assertTrue(wait(lambda: self.u.state == xu.CURRENT))
            mgr.tick(p)
            self.assertEqual((p['/Update/State'], p['/Update/Latest'], p['/Update/Installed'], p['/Update/InstalledBuild'],
                              p['/Update/Check']), (xu.CURRENT, '0.3', '0.3', B, None))
            self.assertFalse(mgr.on_write('/Update/Install', R03))   # not newer
        finally:
            mgr.stop()

    def test_default_updater_reads_the_app_version(self):
        sys.path.insert(0, HERE)
        import test_bridge as tb
        xc = tb.xc
        mgr = xc.Manager(tb.Store(), '', xc.Discovery(browse=False, sweep_hosts=lambda: []), lambda uid, inst: tb.Store(),
                         cache_dir=self.t)
        self.assertEqual((mgr.updater.version, mgr.updater.work), (xc.VERSION, os.path.join(self.t, 'install')))
        with open(GXAPP + '/xregulator/VERSION') as f:
            self.assertEqual(xc.VERSION, f.read().strip())
        self.assertEqual((xc.APP_DIR, mgr.updater.build), (GXAPP + '/xregulator', 0))   # the source tree has no BUILD


BRIDGE_PROC = r'''
# a stand-in for the bridge on the fake GX: a real Updater that installs with the real detached spawn, then waits to be
# killed by install.sh's restart of the service (the svc shim)
import os, sys, time
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import xreg_update as xu
app, work, base, keyfile, want = sys.argv[2:7]
u = xu.Updater(xu.app_version(app), app, work, base=base, key=xu.load_public_key(open(keyfile).read()))
xu.mark_running(work, u.rid)
u.check()
while u.state == xu.CHECKING:
    time.sleep(0.05)
print('checked', u.state, u.latest, flush=True)
print('install', u.install(want), flush=True)
while True:
    time.sleep(0.2)
    print('state', u.state, u.message, flush=True)
'''


class TestDetachedInstall(unittest.TestCase):
    # install.sh restarts the bridge that started it: the install must carry on and finish (or roll back) on its own
    def run_update(self, pkg, want, broken=''):
        gx = sim.GX()
        srv = None
        try:
            self.assertEqual(gx.reboot(PKG), 'unpacked')
            self.assertEqual(gx.running_release(), R03)
            gx.age()
            files = {}
            release(files, pkg)
            srv = Server(files)
            gx.env['SIM_BROKEN'] = broken
            script = os.path.join(gx.root, 'bridge_proc.py')
            with open(script, 'w') as f:
                f.write(BRIDGE_PROC)
            # this process takes the fake bridge's place under the fake supervise, its pid in first so the fake
            # svscan never restarts the one it replaces
            pidf = os.path.join(gx.svc, 'supervise', 'pid')
            with open(pidf) as f:
                old = int(f.read())
            proc = subprocess.Popen([sys.executable, '-B', script, BRIDGE, gx.app, gx.work, srv.base,
                                     os.path.join(KEYS, 'test.pub'), want], env=gx.env, stdout=subprocess.PIPE, text=True)
            with open(pidf, 'w') as f:
                f.write(str(proc.pid))
            os.kill(old, 9)
            rc = proc.wait(timeout=60)
            out = proc.stdout.read()
            self.assertEqual(rc, -15, out)   # SIGTERM from install.sh's svc -t
            self.assertIn('checked 3 ' + want, out)
            # the confirm waits without the lock, so the first install's result stands until pending goes
            self.assertTrue(wait(lambda: os.path.exists(os.path.join(gx.work, 'result'))
                                 and not os.path.exists(os.path.join(gx.work, 'pending'))
                                 and not os.path.exists(os.path.join(gx.work, 'lock')), 40))
            return gx, gx.result_full()
        finally:
            if srv:
                srv.close()

    def test_update_survives_the_restart(self):
        gx, res = self.run_update(sim.bump(PKG, '0.4', os.path.join(KEYS, 'p4.zip')), '0.4+%d' % B)
        try:
            self.assertEqual((res['result'], res['to'], res['from'], res['note']), ('ok', '0.4', '0.3', ''))
            self.assertRegex(res['id'], r'^[0-9a-f]{8}$')
            self.assertEqual((gx.installed(), gx.running()), ('0.4', '0.4'))
            self.assertEqual(gx.read('xregulator', 'install', 'prev', 'VERSION'), '0.3\n')
        finally:
            gx.close()

    def test_reissue_survives_the_restart(self):
        gx, res = self.run_update(sim.bump(PKG, '0.3', os.path.join(KEYS, 'p3b.zip'), build=B + 1), '0.3+%d' % (B + 1))
        try:
            self.assertEqual((res['result'], res['toBuild'], res['fromBuild']), ('ok', B + 1, B))
            self.assertEqual(gx.running_release(), '0.3+%d' % (B + 1))
        finally:
            gx.close()

    def test_rollback_after_the_restart(self):
        gx, res = self.run_update(sim.bump(PKG, '0.4', os.path.join(KEYS, 'p4.zip')), '0.4+%d' % B, broken='0.4')
        try:
            self.assertEqual((res['result'], res['to'], res['from'], res['note']),
                             ('rolledback', '0.4', '0.3', 'The new version did not start within 8 s. The previous '
                                                          'version is back and running.'))
            self.assertEqual((gx.installed(), gx.running()), ('0.3', '0.3'))
            self.assertFalse(os.path.exists(os.path.join(gx.work, 'prev')))
            self.assertEqual(gx.read('xregulator', 'install', 'failed_version'), '0.4+%d\n' % B)
        finally:
            gx.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
