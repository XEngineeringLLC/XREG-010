#!/usr/bin/env python3
# install.sh corners on the fake GX of gx_app/tools/sim_install.py (dash, daemontools shims, Venus OS boot order, a fake
# /proc/uptime): power lost inside the swap (with and without an older stick in) and before the confirmation, the
# confirmation left from an earlier boot, custom startup scripts disabled, a stale lock, a lock held past the wait, a
# short write of rc.local and a stray temp copy of it, an incomplete tree or missing FILES, running it twice, an update that
# leaves the plugin alone, a failed record beside a repair, a refused stick beside an unclaimed install at boot step 30,
# a lock wait that Remove or a full /data ends without spinning, and BUILD parsing identical in install.sh and
# xreg_update.py.
# Run: python3.12 -B test_install_edges.py
import os, resource, shutil, subprocess, sys, tempfile, time, unittest

sys.dont_write_bytecode = True
GXAPP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, GXAPP + '/tools')
sys.path.insert(0, GXAPP)
import sim_install as sim
import build_package as bp
import xreg_update as xu

T = tempfile.mkdtemp(prefix='xreg_edges_')
PKG = os.path.join(T, 'venus-data.zip')
B = 1761800000


def setUpModule():
    bp.build_zip(PKG, '0.3', B)


def tearDownModule():
    shutil.rmtree(T, ignore_errors=True)


class Edges(unittest.TestCase):
    def setUp(self):
        self.gx = sim.GX()

    def tearDown(self):
        self.gx.close()

    def install03(self):
        self.assertEqual(self.gx.reboot(PKG), 'unpacked')
        self.assertEqual(self.gx.running(), '0.3')

    def pkg(self, version, **kw):
        return sim.bump(PKG, version, os.path.join(self.gx.root, 'p%s_%d.zip' % (version, len(os.listdir(self.gx.root)))),
                        **kw)

    def stage(self, version, **kw):
        new = os.path.join(self.gx.work, 'new')
        xu.stage(self.pkg(version, **kw), new)
        return new

    def pending(self, to, frm, boot):
        with open(os.path.join(self.gx.work, 'pending'), 'w') as f:
            f.write('%s+%d %s+%d %s -\n' % (to, B, frm, B, boot))

    def test_power_lost_between_the_two_renames(self):
        gx = self.gx
        self.install03()
        for stick in (None, self.pkg('0.2.9')):
            self.stage('0.4')
            # what install.sh had done when the power went: pending written, the app moved aside, the new one not in yet
            self.pending('0.4', '0.3', gx.env['XREG_BOOT_ID'])
            shutil.move(gx.app, os.path.join(gx.work, 'prev'))
            out = gx.reboot(stick)
            # with an older stick in, the pre-hook puts the app back first and skips the stick
            self.assertEqual((gx.installed(), gx.running(), out), ('0.3', '0.3', 'skipped' if stick else None),
                             gx.transcript[-4:])
            r = gx.result()
            self.assertEqual(r[:3], ('failed', '0.4', '0.3'))
            self.assertIn('interrupted; the previous version is in place', r[3])
            self.assertFalse(os.path.exists(os.path.join(gx.work, 'pending')))

    def test_power_lost_before_the_confirmation(self):
        gx = self.gx
        self.install03()
        for broken, want, res in (('', '0.4', 'ok'), ('0.5', '0.4', 'rolledback')):
            v = '0.4' if not broken else '0.5'
            new = self.stage(v)
            gx.env['SIM_BROKEN'] = broken
            # install.sh up to the restart, as the stick's post-hook leaves it before svscan runs: the swap, rc.local,
            # the service copy, the link, and a detached confirm, which the power cut takes with it
            gx.svscan_off()
            r = gx.sh(os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('a detached install.sh confirm waits', r.stdout)
            subprocess.run(['pkill', '-9', '-f', gx.root + '/data/apps/available/xregulator/install.sh confirm'])
            self.assertTrue(os.path.exists(os.path.join(gx.work, 'pending')))
            gx.reboot()   # the next boot's rc.local line confirms or rolls back
            self.assertEqual((gx.installed(), gx.running(), gx.result()[:2]), (want, want, (res, v)), gx.transcript[-3:])
            gx.env['SIM_BROKEN'] = ''

    def test_confirmation_resumed_by_a_stick_without_rc_local(self):
        # custom startup scripts disabled, the power cut while the stick install waited for its bridge: the same stick
        # at the next boot finishes the confirmation (rc.local does not run)
        gx = self.gx
        self.install03()
        os.rename(gx.rc, gx.rc + '.disabled')
        p4 = self.pkg('0.4')
        new = os.path.join(gx.work, 'new')
        xu.stage(p4, new)
        gx.svscan_off()
        gx.sh(os.path.join(new, 'install.sh'), 'install', new)
        subprocess.run(['pkill', '-9', '-f', gx.root + '/data/apps/available/xregulator/install.sh confirm'])
        self.assertTrue(os.path.exists(os.path.join(gx.work, 'pending')))
        out = gx.reboot(p4)
        r = gx.result()
        self.assertEqual((out, gx.running(), r[:3]), ('skipped', '0.4', ('ok', '0.4', '0.3')), gx.transcript[-4:])
        self.assertIn('Custom startup scripts are disabled', r[3])

    def test_resumed_confirmation_makes_the_link(self):
        # the power cut after the plugin link was removed and before it was made again, an older stick in: the
        # pre-hook resumes the confirmation, and rc.local's boot leaves the boot work to it
        gx = self.gx
        self.install03()
        new = self.stage('0.4')
        gx.svscan_off()
        gx.sh(os.path.join(new, 'install.sh'), 'install', new)
        subprocess.run(['pkill', '-9', '-f', gx.root + '/data/apps/available/xregulator/install.sh confirm'])
        os.remove(gx.link)
        out = gx.reboot(self.pkg('0.2.9'))
        self.assertEqual((out, gx.running(), gx.result()[:2]), ('skipped', '0.4', ('ok', '0.4')), gx.transcript[-4:])
        self.assertTrue(os.path.islink(gx.link) and os.readlink(gx.link) == gx.app)

    def test_damage_check_without_sha256sum(self):
        # a busybox without sha256sum (Yocto releases after scarthgap leave it out): the check falls back to python3
        gx = self.gx
        self.install03()
        os.remove(os.path.join(gx.bin, 'sha256sum'))
        gx.env['PATH'] = gx.bin + ':/usr/bin:/bin'
        self.assertIsNone(shutil.which('sha256sum', path=gx.env['PATH']))
        self.assertEqual(gx.reboot(PKG), 'skipped')                       # healthy: the same stick is skipped
        with open(os.path.join(gx.app, 'bridge', 'xreg_cloud.py'), 'a') as f:
            f.write('# changed\n')
        self.assertEqual(gx.reboot(PKG), 'unpacked', gx.transcript[-3:])  # a changed file: the same stick repairs it
        self.assertEqual(gx.running(), '0.3')
        with open(os.path.join(gx.app, 'bridge', 'xreg_cloud.py')) as f:
            self.assertNotIn('# changed', f.read())

    def test_stray_rc_temp_cleaned(self):
        # the power cut inside rc_commit, after the copy and before the rename
        gx = self.gx
        self.install03()
        with open(gx.rc + '.xregulator-new', 'w') as f:
            f.write('half\n')
        gx.reboot()
        self.assertFalse(os.path.exists(gx.rc + '.xregulator-new'))
        self.assertEqual(gx.running(), '0.3')

    def test_missing_files_list_is_damage(self):
        gx = self.gx
        self.install03()
        os.remove(os.path.join(gx.app, 'FILES'))
        self.assertEqual(gx.reboot(PKG), 'unpacked', gx.transcript[-3:])
        self.assertTrue(os.path.isfile(os.path.join(gx.app, 'FILES')))
        self.assertEqual(gx.reboot(PKG), 'skipped')

    def test_failed_record_never_blocks_a_repair(self):
        # failed_version naming the installed release (from a push of a release that once failed) does not stop the
        # same release's stick from repairing it; it only stops a stick from retrying a release that is not installed
        gx = self.gx
        self.install03()
        with open(os.path.join(gx.work, 'failed_version'), 'w') as f:
            f.write('0.3+%d\n' % B)
        with open(gx.rc, 'w') as f:
            f.write('#!/bin/sh\n')
        gx.reboot()
        self.assertIsNone(gx.running(2))
        self.assertEqual(gx.reboot(PKG), 'unpacked', gx.transcript[-3:])
        self.assertEqual(gx.running(), '0.3')

    def test_refused_stick_and_unclaimed_install_do_not_hold_step_30(self):
        # an install of an earlier boot never confirmed, and a stick the pre-hook refuses: the pre-hook starts a confirm
        # and returns at once
        gx = self.gx
        self.install03()
        new = self.stage('0.4')
        gx.svscan_off()
        gx.sh(os.path.join(new, 'install.sh'), 'install', new)
        subprocess.run(['pkill', '-9', '-f', gx.root + '/data/apps/available/xregulator/install.sh confirm'])
        out = gx.reboot([self.pkg('0.2.9'), self.pkg('0.2.8')])
        self.assertEqual(out, 'skipped')
        self.assertLess(max(gx.unpack_s), 5, gx.unpack_s)
        self.assertEqual((gx.running(), gx.result()[:2]), ('0.4', ('ok', '0.4')))

    def test_build_strings_read_alike(self):
        # install.sh's build_of and xreg_update.app_build take exactly the same BUILD files and refuse the rest
        with open(os.path.join(GXAPP, 'xregulator', 'install.sh')) as f:
            src = f.read()
        fn = src[src.index('build_of() {'):]
        fn = fn[:fn.index('\n}\n') + 3]
        cases = [b'0', b'0\n', b'1', b'7\n', b'1761800000\n', b'999999999999', b'1000000000000', b'00', b'007', b'',
                 b'\n', b'\n7', b' 7', b'7 ', b'7\r\n', b'+7', b'-7', b'7\n8', '\uff17'.encode(), '\u0667'.encode(),
                 b'0x7', b'7.0', None]
        d = tempfile.mkdtemp(dir=T)
        for c in cases:
            if os.path.exists(os.path.join(d, 'BUILD')):
                os.remove(os.path.join(d, 'BUILD'))
            if c is not None:
                with open(os.path.join(d, 'BUILD'), 'wb') as f:
                    f.write(c)
            r = subprocess.run([sim.SHELL, '-c', fn + 'build_of "$1"', 'sh', d], capture_output=True, text=True)
            shell = int(r.stdout) if r.returncode == 0 else None
            self.assertEqual(shell, xu.app_build(d), repr(c))
        self.assertEqual(xu.app_build(d), 0)   # no BUILD file

    def test_lock_wait_ends_with_a_result(self):
        gx = self.gx
        self.install03()
        holder = subprocess.Popen(['sleep', '30'])
        try:
            os.makedirs(os.path.join(gx.work, 'lock'))
            with open(os.path.join(gx.work, 'lock', 'owner'), 'w') as f:
                f.write('%d %s\n' % (holder.pid, gx.env['XREG_BOOT_ID']))
            with open(os.path.join(gx.work, 'pending'), 'w') as f:
                f.write('other\n')
            new = self.stage('0.4')
            body = open(os.path.join(new, 'install.sh')).read().replace('\twait_s=600\n', '\twait_s=3\n', 1)
            self.assertIn('wait_s=3', body)
            with open(os.path.join(new, 'install.sh'), 'w') as f:
                f.write(body)
            r = gx.sh(os.path.join(new, 'install.sh'), 'install', new)
            self.assertEqual(r.returncode, 1)
            res = gx.result()
            self.assertEqual((res[0], res[1], res[2]), ('failed', '0.4', '0.3'))
            self.assertIn('Another install did not finish', res[3])
            self.assertTrue(os.path.exists(os.path.join(gx.work, 'pending')))   # the running install's, left alone
            self.assertTrue(os.path.isdir(os.path.join(gx.work, 'lock')))
        finally:
            holder.kill()

    def child_cpu(self):
        r = resource.getrusage(resource.RUSAGE_CHILDREN)
        return r.ru_utime + r.ru_stime

    def test_remove_during_a_lock_wait_ends_it(self):
        # Remove deletes /data/xregulator, lock folder and all, while an install waits on the lock
        gx = self.gx
        self.install03()
        holder = subprocess.Popen(['sleep', '30'])
        try:
            os.makedirs(os.path.join(gx.work, 'lock'))
            with open(os.path.join(gx.work, 'lock', 'owner'), 'w') as f:
                f.write('%d %s\n' % (holder.pid, gx.env['XREG_BOOT_ID']))
            new = self.stage('0.4')
            cpu0 = self.child_cpu()
            p = subprocess.Popen([sim.SHELL, os.path.join(new, 'install.sh'), 'install', new], env=gx.env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.5)
            self.assertIsNone(p.poll())   # waiting on the live lock
            shutil.rmtree(os.path.dirname(gx.work))
            self.assertEqual(p.wait(timeout=10), 1)
            self.assertLess(self.child_cpu() - cpu0, 1.0)
            self.assertEqual(gx.installed(), '0.3')
        finally:
            holder.kill()
            holder.wait()

    def test_lock_that_cannot_be_made_waits_out_its_time(self):
        # a full /data: mkdir of the lock fails with no lock there; the wait sleeps to its end instead of spinning
        gx = self.gx
        self.install03()
        new = os.path.join(gx.root, 'new04')
        xu.stage(self.pkg('0.4'), new)
        body = open(os.path.join(new, 'install.sh')).read().replace('\twait_s=600\n', '\twait_s=3\n', 1)
        self.assertIn('wait_s=3', body)
        with open(os.path.join(new, 'install.sh'), 'w') as f:
            f.write(body)
        os.chmod(gx.work, 0o555)
        try:
            cpu0, t0 = self.child_cpu(), time.time()
            r = gx.sh(os.path.join(new, 'install.sh'), 'install', new)
            self.assertEqual(r.returncode, 1)
            self.assertGreater(time.time() - t0, 1.5)   # two 1 s sleeps before the third try ends it
            self.assertLess(self.child_cpu() - cpu0, 1.0)
        finally:
            os.chmod(gx.work, 0o755)
        self.assertEqual(gx.running(), '0.3')

    def test_short_write_keeps_rc_local(self):
        # a full /data: the rebuilt rc.local comes out short; the owner's file stays as it was
        gx = self.gx
        owner = '#!/bin/sh\necho one\necho two\n'
        with open(gx.rc, 'w') as f:
            f.write(owner)
        os.chmod(gx.rc, 0o755)
        with open(os.path.join(gx.bin, 'awk'), 'w') as f:
            f.write('#!/bin/sh\ncase $2 in line=*) /usr/bin/awk "$@" | head -n 1; exit 0 ;; esac\nexec /usr/bin/awk "$@"\n')
        os.chmod(os.path.join(gx.bin, 'awk'), 0o755)
        gx.reboot(PKG)
        self.assertEqual(gx.read('rc.local'), owner)
        self.assertEqual(sorted(f for f in os.listdir(gx.data) if f.startswith('rc.local')), ['rc.local'])
        r = gx.result()
        self.assertEqual(r[0], 'ok')   # the app runs this boot
        self.assertIn('could not be written to /data/rc.local', r[3])

    def test_custom_startup_scripts_disabled_in_app(self):
        gx = self.gx
        self.install03()
        os.rename(gx.rc, gx.rc + '.disabled')   # Support status > Disable custom startup scripts
        with open(gx.rc + '.disabled', 'a') as f:
            f.write('echo owner\n')
        gx.reboot()
        self.assertIsNone(gx.running(2))          # nothing starts the bridge
        self.assertTrue(os.path.islink(gx.link))  # the pages still load
        gx.svscan_off()
        gx.svscan_on()
        new = self.stage('0.4')
        r = gx.sh(os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only')   # the in-app path, this session
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(gx.rc))
        dl = gx.read('rc.local.disabled').splitlines()
        self.assertEqual((dl[0], dl[-1]), ('#!/bin/sh', 'echo owner'))
        self.assertTrue(dl[1].endswith('# xregulator-app') and len(gx.rc_lines(gx.rc + '.disabled')) == 1)
        self.assertEqual(gx.running(), '0.4')
        self.assertEqual(gx.result()[3], sim.HOOKS_OFF)
        os.rename(gx.rc + '.disabled', gx.rc)    # Re-enable custom startup scripts
        gx.reboot()
        self.assertEqual(gx.running(), '0.4')

    def test_old_line_and_twice(self):
        gx = self.gx
        self.install03()
        with open(gx.rc, 'a') as f:
            f.write('[ -d %s ] || cp -a %s/service %s\n' % (gx.svc, gx.app, gx.svc))   # the pre-0.3 line
        for _ in range(2):
            r = gx.sh(os.path.join(gx.app, 'install.sh'))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(len(gx.rc_lines()), 1)
        self.assertTrue(os.path.islink(gx.link) and os.path.isdir(gx.svc))
        self.assertEqual(gx.running(), '0.3')

    def test_stale_lock_and_incomplete_tree(self):
        gx = self.gx
        self.install03()
        os.makedirs(os.path.join(gx.work, 'lock'))
        with open(os.path.join(gx.work, 'lock', 'owner'), 'w') as f:
            f.write('1 boot0\n')   # left by an install.sh of an earlier boot
        new = self.stage('0.4')
        os.remove(os.path.join(new, 'bridge', 'xreg_update.py'))
        before = gx.tree_hash(gx.app)
        r = gx.sh(os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only')
        self.assertEqual(r.returncode, 1)
        self.assertEqual(gx.result()[:3], ('failed', '0.4', '0.3'))
        self.assertIn('bridge/xreg_update.py', gx.result()[3])
        self.assertEqual((gx.tree_hash(gx.app), gx.running()), (before, '0.3'))
        self.assertFalse(os.path.exists(new) or os.path.exists(os.path.join(gx.work, 'lock')))

    def test_live_lock_waits(self):
        gx = self.gx
        self.install03()
        os.makedirs(os.path.join(gx.work, 'lock'))
        holder = subprocess.Popen(['sleep', '4'])
        with open(os.path.join(gx.work, 'lock', 'owner'), 'w') as f:
            f.write('%d %s\n' % (holder.pid, gx.env['XREG_BOOT_ID']))
        new = self.stage('0.4')
        t0 = time.time()
        p = subprocess.Popen([sim.SHELL, os.path.join(new, 'install.sh'), 'install', new], env=gx.env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(1)
        self.assertEqual(gx.installed(), '0.3')   # still waiting on the lock
        holder.wait()
        self.assertEqual(p.wait(timeout=60), 0)
        p.stdout.close()
        self.assertGreater(time.time() - t0, 3.5)
        self.assertEqual(gx.running(), '0.4')

    def test_unchanged_plugin_keeps_the_link(self):
        gx = self.gx
        self.install03()
        ino = os.lstat(gx.link).st_ino
        new = self.stage('0.4', plugin=False)
        r = gx.sh(os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only')
        self.assertEqual((r.returncode, gx.running()), (0, '0.4'))
        self.assertEqual(os.lstat(gx.link).st_ino, ino)    # gui-v2 left alone
        new = self.stage('0.5')                             # a changed plugin: the link is made again
        r = gx.sh(os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only')
        self.assertEqual((r.returncode, gx.running()), (0, '0.5'))
        self.assertNotEqual(os.lstat(gx.link).st_ino, ino)
        self.assertEqual(os.readlink(gx.link), gx.app)


if __name__ == '__main__':
    unittest.main(verbosity=2)
