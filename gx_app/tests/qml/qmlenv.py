# Shared setup of the offline QML suites: where gui-v2 1.3.22 is (the download cache that ../fetch_deps.sh fills), and
# the stub Victron.VenusOS module, put together in a work folder outside the repo because each run writes its Theme.qml.
# XREG_TEST_CACHE moves the cache, XREG_GUIV2 points at another gui-v2 tree, XREG_TEST_WORK names the work folder.
import atexit, os, shutil, subprocess, sys, tempfile

QML = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(QML)))
SCHEMA = os.path.join(REPO, 'gx_app', 'xregulator', 'bridge', 'settings_schema.json')
FIXTURES = os.path.join(QML, 'fixtures')
CACHE = os.environ.get('XREG_TEST_CACHE') or os.path.expanduser('~/Library/Caches/xreg-gx-tests')
GUIV2 = os.environ.get('XREG_GUIV2') or os.path.join(CACHE, 'gui-v2-1.3.22')
FONT = os.path.join(GUIV2, 'fonts', 'Roboto-Regular.ttf')

# gui-v2's own files in the stub module, taken from the cache rather than kept in the repo
UPSTREAM = {'switch_indicator.png': 'images/switch_indicator.png',
            'icon_chevron_right_32.svg': 'images/icon_chevron_right_32.svg'}
# Each variant: the folders laid over stubs/, then gui-v2 files of its own
VARIANTS = {
    'base': ((), {}),
    'switch': (('stubs_switch',), {'ListSwitch.qml': 'components/listitems/core/ListSwitch.qml'}),
    'timing': (('stubs_timing',), {}),
}


def need_guiv2():
    if not os.path.isfile(os.path.join(GUIV2, 'themes', 'color', 'Dark.json')):
        sys.exit('gui-v2 1.3.22 is not at %s: run gx_app/tests/fetch_deps.sh' % GUIV2)


def work_dir():
    w = os.environ.get('XREG_TEST_WORK')
    if not w:
        w = tempfile.mkdtemp(prefix='xreg-qml-')
        atexit.register(shutil.rmtree, w, True)
    os.makedirs(w, exist_ok=True)
    return w


def stubs(size='SevenInch', scheme='Dark', variant='base'):
    """A fresh copy of the stub module for one screen size and colour scheme; returns the folder to add as import path."""
    need_guiv2()
    overlays, upstream = VARIANTS[variant]
    d = tempfile.mkdtemp(prefix='stubs_%s_%s_%s_' % (variant, size, scheme), dir=work_dir())
    shutil.copytree(os.path.join(QML, 'stubs'), d, dirs_exist_ok=True)
    for o in overlays:
        shutil.copytree(os.path.join(QML, o), d, dirs_exist_ok=True)
    vos = os.path.join(d, 'Victron', 'VenusOS')
    for name, rel in dict(UPSTREAM, **upstream).items():
        shutil.copyfile(os.path.join(GUIV2, rel), os.path.join(vos, name))
    subprocess.check_call([sys.executable, os.path.join(QML, 'gen_theme.py'), size, scheme, os.path.join(vos, 'Theme.qml')])
    return d
