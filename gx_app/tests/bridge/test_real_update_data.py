#!/usr/bin/env python3
# Victron's own update-data.sh (meta-victronenergy v3.85, fetched by ../fetch_deps.sh) run on the built venus-data.zip
# under dash, with only /media and /data pointed at a fake GX (gx_app/tools/sim_install.py) and Info-ZIP unzip standing
# in for busybox's: fresh install, stick left in, an in-app update then the stick left in, a corrupt payload, removal.
# Run: python3.12 -B test_real_update_data.py
import os, shutil, subprocess, sys, tempfile, unittest, zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
GXAPP = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, GXAPP + '/tools')
sys.path.insert(0, GXAPP)
import sim_install as sim
import build_package as bp

SRC = os.path.join(os.environ.get('XREG_TEST_CACHE') or os.path.expanduser('~/Library/Caches/xreg-gx-tests'),
                   'update-data_v3.85.sh')


class RealGX(sim.GX):
    def __init__(self):
        super().__init__()
        self.media = os.path.join(self.root, 'media')
        os.makedirs(os.path.join(self.media, 'sda1'))
        with open(SRC) as f:
            s = f.read()
        assert s.count('for dir in /media/*; do') == 1 and s.count('unpack "$archive" /data') == 1
        s = s.replace('for dir in /media/*; do', 'for dir in %s/*; do' % self.media)
        s = s.replace('unpack "$archive" /data', 'unpack "$archive" %s' % self.data)
        self.script = os.path.join(self.root, 'update-data.sh')
        with open(self.script, 'w') as f:
            f.write(s)
        self.out = ''

    def update_data(self, archive):
        stick = os.path.join(self.media, 'sda1')
        for f in os.listdir(stick):
            os.remove(os.path.join(stick, f))
        shutil.copyfile(archive, os.path.join(stick, 'venus-data.zip'))
        r = self.sh(self.script)
        self.out = r.stdout + r.stderr
        if 'skipped' in self.out:
            return 'skipped'
        return 'unpacked' if 'Updating' in self.out else 'none'


class TestRealUpdateData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.mkdtemp()
        cls.pkg = os.path.join(cls.t, 'venus-data.zip')
        bp.build_zip(cls.pkg, '0.3', 1761800000)
        cls.p4 = sim.bump(cls.pkg, '0.4', os.path.join(cls.t, 'p4.zip'))
        cls.p3b = sim.bump(cls.pkg, '0.3', os.path.join(cls.t, 'p3b.zip'), build=1761800100)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.t, ignore_errors=True)

    def test_sequence(self):
        gx = RealGX()
        try:
            self.assertEqual(gx.reboot(self.pkg), 'unpacked', gx.out)
            self.assertEqual((gx.installed(), gx.running(), gx.result()), ('0.3', '0.3', ('ok', '0.3', '', '')), gx.out)
            self.assertIn('USB/SD package 0.3+1761800000 admitted over nothing', gx.out)
            self.assertFalse(os.path.exists(os.path.join(gx.work, 'new')))
            mode = lambda rel: oct(os.stat(os.path.join(gx.app, rel)).st_mode & 0o777)
            self.assertEqual([mode(r) for r in ('install.sh', 'service/run', 'bridge/xregulator_bridge.py',
                                                'bridge/xreg_core.py', 'gui-v2/xregulator.json')],
                             ['0o755', '0o755', '0o755', '0o644', '0o644'])
            before = gx.tree_hash(gx.app)
            self.assertEqual(gx.reboot(self.pkg), 'skipped', gx.out)
            self.assertIn('0.3+1761800000 installed; USB/SD package 0.3+1761800000 skipped', gx.out)
            self.assertEqual((gx.tree_hash(gx.app), gx.running()), (before, '0.3'))
            self.assertEqual(gx.in_app(self.p4), 0)
            self.assertEqual((gx.installed(), gx.running()), ('0.4', '0.4'))
            self.assertEqual(gx.reboot(self.pkg), 'skipped', gx.out)   # the 0.3 stick still in after the in-app update
            self.assertIn('0.4+1761800000 installed; USB/SD package 0.3+1761800000 skipped', gx.out)
            self.assertEqual((gx.installed(), gx.running(), gx.result()[:3]), ('0.4', '0.4', ('ok', '0.4', '0.3')))
            self.assertEqual(gx.remove(), 0)
            self.assertEqual(gx.reboot(self.pkg), 'unpacked', gx.out)   # removed, stick still in: installs again
            self.assertEqual((gx.installed(), gx.running()), ('0.3', '0.3'))
            # a version issued again: the newer build goes in, the older stick is then skipped
            self.assertEqual(gx.reboot(self.p3b), 'unpacked', gx.out)
            self.assertEqual(gx.running_release(), '0.3+1761800100')
            self.assertEqual(gx.reboot(self.pkg), 'skipped', gx.out)
            # another add-on rewrites rc.local: the same stick repairs the install
            with open(gx.rc, 'w') as f:
                f.write('#!/bin/sh\necho other >> /dev/null\n')
            gx.reboot()
            self.assertIsNone(gx.running(2))
            self.assertEqual(gx.reboot(self.p3b), 'unpacked', gx.out)
            self.assertIn('is damaged; USB/SD package 0.3+1761800100 admitted to repair it', gx.out)
            self.assertEqual((gx.running_release(), len(gx.rc_lines())), ('0.3+1761800100', 1))
        finally:
            gx.close()

    def test_corrupt_payload(self):
        # a member of the app tree with a bad CRC: unzip fails, the post-hook gets extraction-failed
        bad = os.path.join(self.t, 'bad.zip')
        with open(self.p4, 'rb') as f:
            data = bytearray(f.read())
        with zipfile.ZipFile(self.p4) as z:
            zi = z.getinfo(sim.PAYLOAD + 'bridge/xreg_core.py')
            start = zi.header_offset + 30 + len(zi.filename.encode()) + len(zi.extra)
        data[start + 200] ^= 0xff
        with open(bad, 'wb') as f:
            f.write(data)
        gx = RealGX()
        try:
            gx.reboot(self.pkg)
            self.assertEqual(gx.installed(), '0.3')
            gx.reboot(bad)
            self.assertEqual((gx.installed(), gx.running()), ('0.3', '0.3'), gx.out)
            self.assertEqual(gx.result(), ('failed', '0.4', '0.3', 'The USB/SD package did not unpack (extraction-failed).'),
                             gx.out)
            self.assertFalse(os.path.exists(os.path.join(gx.work, 'pending')))
            self.assertFalse(os.path.exists(os.path.join(gx.work, 'new')))
        finally:
            gx.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
