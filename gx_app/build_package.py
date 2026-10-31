#!/usr/bin/env python3
# Builds the GX app package from gx_app/xregulator, versioned from xregulator/VERSION and stamped with a build (the
# build time in epoch seconds; XREG_BUILD overrides), so a version number issued again still reaches every GX. One zip
# serves both install paths:
#   venus-data.zip               an owner copies it to a USB stick or SD card; Venus OS's update-data.sh unpacks it into
#                                /data at boot and runs its rc/ hooks (rc/install.sh is a copy of the app's install.sh)
#   gxapp_<version>_<sha12>.zip  the same bytes, named by content so no published file is ever replaced (the OTA host
#                                lets caches keep a zip for an hour), for the in-app updater (xreg_update.py), with
#                                gxapp_<version>_<sha12>.sig (base64 RSA SHA-256 signature, as build_and_deploy.sh signs
#                                firmware), gxapp_versions.json, and release.json naming the files for publish_gx.sh
# The app tree sits at xregulator/install/new/ in the zip, so the stick path unpacks it to /data/xregulator/install/new,
# where install.sh takes it from, the same place the updater stages a download. VERSION, BUILD and FILES (the sha256 of
# every app file, which install.sh checks to tell a damaged install) are in the app tree and in rc/.
#   python3 build_package.py <out dir> [--key private.pem [--ota-key]] [--base-manifest json] [--notes text]
# Without --key: the zip only. --ota-key refuses a key whose public half is not the bridge's OTA_PUBLIC_KEY. A version the
# manifest lists already gets its entry replaced by this build. Every build runs tools/sim_install.py against the zip
# and writes nothing if it fails.
import argparse, base64, hashlib, json, os, re, shutil, stat, subprocess, sys, tempfile, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.join(HERE, 'xregulator')
HOOKS = os.path.join(HERE, 'package', 'rc')
PAYLOAD = 'xregulator/install/new/'
EXECUTABLE = {'install.sh', 'service/run', 'service/log/run', 'bridge/xregulator_bridge.py'}
SKIP = re.compile(r'(^|/)(__pycache__|\.DS_Store)(/|$)|\.pyc$|^(BUILD|FILES)$')   # BUILD and FILES are made here
sys.dont_write_bytecode = True   # no __pycache__ in the app tree
sys.path.insert(0, os.path.join(TREE, 'bridge'))
import xreg_update   # noqa: E402  the bridge's own version rules and signature check


def die(msg):
    print('build_package: ' + msg, file=sys.stderr)
    sys.exit(1)


def tree_files():
    out = []
    for d, dirs, files in os.walk(TREE):
        dirs[:] = sorted(x for x in dirs if not SKIP.search(x + '/'))
        for f in sorted(files):
            rel = os.path.relpath(os.path.join(d, f), TREE)
            if not SKIP.search(rel):
                out.append(rel)
    return out


def entry(name, mode, when):
    zi = zipfile.ZipInfo(name, date_time=when)
    zi.create_system = 3   # Unix: busybox unzip takes the mode from external_attr only then
    zi.external_attr = mode << 16
    if name.endswith('/'):
        zi.external_attr |= 0x10
    else:
        zi.compress_type = zipfile.ZIP_DEFLATED
    return zi


def files_list(blobs):
    # "<sha256>  <path>" per app file, as sha256sum prints it, for install.sh's check of an installed app
    return ''.join('%s  %s\n' % (hashlib.sha256(blobs[rel]).hexdigest(), rel) for rel in sorted(blobs)).encode()


def build_zip(path, version, build):
    files = tree_files()
    if any(re.search(r'\s', rel) for rel in files):
        die('a file name in the app tree has a space: install.sh lists files by name')
    # dated by the newest input (SOURCE_DATE_EPOCH overrides), so the same tree builds the same bytes
    stamp = int(os.environ.get('SOURCE_DATE_EPOCH') or max(
        os.path.getmtime(p) for p in [os.path.join(TREE, f) for f in files] + [os.path.join(HOOKS, h) for h in os.listdir(HOOKS)]))
    when = time.gmtime(max(stamp, 315532800))[:6]
    dirs = sorted({os.path.dirname(f) for f in files} - {''})
    dirs = sorted({'/'.join(d.split('/')[:i + 1]) for d in dirs for i in range(len(d.split('/')))})
    blobs = {}
    for rel in files:
        with open(os.path.join(TREE, rel), 'rb') as f:
            blobs[rel] = f.read()
    blobs['BUILD'] = b'%d\n' % build
    listing = files_list(blobs)
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr(entry('rc/', stat.S_IFDIR | 0o755, when), b'')
        for name in ('pre-hook.sh', 'post-hook.sh'):
            with open(os.path.join(HOOKS, name), 'rb') as f:
                z.writestr(entry('rc/' + name, stat.S_IFREG | 0o755, when), f.read())
        with open(os.path.join(TREE, 'install.sh'), 'rb') as f:
            z.writestr(entry('rc/install.sh', stat.S_IFREG | 0o755, when), f.read())
        z.writestr(entry('rc/VERSION', stat.S_IFREG | 0o644, when), (version + '\n').encode())
        z.writestr(entry('rc/BUILD', stat.S_IFREG | 0o644, when), blobs['BUILD'])
        z.writestr(entry('rc/FILES', stat.S_IFREG | 0o644, when), listing)
        z.writestr(entry(PAYLOAD, stat.S_IFDIR | 0o755, when), b'')
        for d in dirs:
            z.writestr(entry(PAYLOAD + d + '/', stat.S_IFDIR | 0o755, when), b'')
        for rel in files + ['BUILD']:
            src = os.path.join(TREE, rel)
            mode = 0o755 if rel in EXECUTABLE or (os.path.exists(src) and os.stat(src).st_mode & 0o111) else 0o644
            z.writestr(entry(PAYLOAD + rel, stat.S_IFREG | mode, when), blobs[rel])
        z.writestr(entry(PAYLOAD + 'FILES', stat.S_IFREG | 0o644, when), listing)


def check_zip(path, rel):
    # what busybox unzip and xreg_update.stage will see: Unix entries, plain modes, no links, the release in both places,
    # FILES true to the app tree
    version, _, build = rel.partition('+')
    with zipfile.ZipFile(path) as z:
        for zi in z.infolist():
            mode = zi.external_attr >> 16
            if zi.create_system != 3 or stat.S_IMODE(mode) not in (0o755, 0o644) or stat.S_ISLNK(mode):
                die('bad zip entry %s (system %d, mode %o)' % (zi.filename, zi.create_system, mode))
            if not (zi.filename.startswith('rc/') or zi.filename.startswith(PAYLOAD)):
                die('zip entry outside rc/ and the payload: ' + zi.filename)
        if xreg_update.package_release(z) != rel or z.read('rc/VERSION').decode().strip() != version \
                or z.read('rc/BUILD').decode().strip() != build:
            die('the package does not carry release ' + rel)
        blobs = {zi.filename[len(PAYLOAD):]: z.read(zi) for zi in z.infolist()
                 if zi.filename.startswith(PAYLOAD) and not zi.filename.endswith('/') and zi.filename != PAYLOAD + 'FILES'}
        if z.read(PAYLOAD + 'FILES') != files_list(blobs) or z.read('rc/FILES') != files_list(blobs):
            die('FILES does not match the app tree in the package')
        for rel in EXECUTABLE:
            if not (z.getinfo(PAYLOAD + rel).external_attr >> 16) & 0o100:
                die(rel + ' is not executable in the zip')


def sign(path, key, out_sig, ota_key):
    with tempfile.TemporaryDirectory() as t:
        raw = os.path.join(t, 'sig')
        subprocess.run(['openssl', 'dgst', '-sha256', '-sign', key, '-out', raw, path], check=True)
        pub = subprocess.run(['openssl', 'rsa', '-in', key, '-pubout'], check=True, capture_output=True,
                             text=True).stdout
        with open(raw, 'rb') as f:
            sig = f.read()
    with open(path, 'rb') as f:
        digest = hashlib.sha256(f.read()).digest()
    pubkey = xreg_update.load_public_key(pub)
    if not xreg_update.verify_digest(digest, sig, pubkey):
        die('the signature does not verify with the bridge\'s own check')
    if ota_key and pubkey != xreg_update.load_public_key(xreg_update.OTA_PUBLIC_KEY):
        die('this key is not the one whose public half the bridge carries (OTA_PUBLIC_KEY)')
    with open(out_sig, 'w') as f:
        f.write(base64.b64encode(sig).decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--key')
    ap.add_argument('--ota-key', action='store_true')
    ap.add_argument('--base-manifest')
    ap.add_argument('--notes', default='')
    a = ap.parse_args()

    version = xreg_update.app_version(TREE)
    if version == '0':
        die('xregulator/VERSION is missing or not a version')
    with open(os.path.join(TREE, 'gui-v2', 'xregulator.json')) as f:
        plugin = json.load(f).get('version')
    if plugin != version:
        die('the plugin is built as %s, VERSION says %s: run build_plugin.sh' % (plugin, version))
    manifest = {'versions': {}}
    if a.base_manifest:
        with open(a.base_manifest) as f:
            manifest = json.load(f)
        if not isinstance(manifest.get('versions'), dict):
            die('%s has no versions object' % a.base_manifest)
    build = int(os.environ.get('XREG_BUILD') or time.time())
    rel = xreg_update.release(version, build)
    listed = [xreg_update.release(v, e.get('build', 0)) for v, e in manifest['versions'].items() if isinstance(e, dict)]
    newest = max(listed, key=lambda r: xreg_update.release_key(r) or ((), 0), default=None)
    if newest and not xreg_update.newer(rel, newest):
        die('the manifest lists %s, not older than %s: GXs would never take this one' % (newest, rel))

    os.makedirs(a.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        zpath = os.path.join(t, 'venus-data.zip')
        build_zip(zpath, version, build)
        check_zip(zpath, rel)
        sim = subprocess.run([sys.executable, os.path.join(HERE, 'tools', 'sim_install.py'), zpath])
        if sim.returncode:
            die('tools/sim_install.py failed: nothing written')
        with open(zpath, 'rb') as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest()
        name = 'gxapp_%s_%s' % (version, sha[:12])
        if a.key:
            sign(zpath, a.key, os.path.join(t, 'sig'), a.ota_key)
            shutil.copyfile(os.path.join(t, 'sig'), os.path.join(a.out, name + '.sig'))
        shutil.copyfile(zpath, os.path.join(a.out, 'venus-data.zip'))
        shutil.copyfile(zpath, os.path.join(a.out, name + '.zip'))
    print('venus-data.zip and %s.zip: release %s, %d bytes, sha256 %s' % (name, rel, len(data), sha))
    if not a.key:
        return
    manifest['versions'][version] = {
        'build': build, 'file': name + '.zip', 'sig': name + '.sig', 'size': len(data), 'sha256': sha,
        'notes': a.notes or 'Version ' + version, 'release_date': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(build))}
    with open(os.path.join(a.out, 'gxapp_versions.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(a.out, 'release.json'), 'w') as f:
        json.dump({'version': version, 'build': build, 'release': rel, 'zip': name + '.zip', 'sig': name + '.sig',
                   'size': len(data), 'sha256': sha}, f, indent=2)
    # the bridge's own reading of what it will fetch
    latest, e = xreg_update.parse_manifest(json.dumps(manifest).encode())
    print('gxapp_versions.json: %d versions, latest %s -> %s, %s' % (len(manifest['versions']), latest, e['file'], e['sig']))


if __name__ == '__main__':
    main()
