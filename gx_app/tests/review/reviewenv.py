# The review harnesses work in one folder outside the repo: a copy of gx_app (upd_test.py writes a BUILD into its app
# tree) and out/venus-data.zip built from that copy as release 0.3+1761800000, the release the harnesses start from.
# XREG_REVIEW_WORK names the folder (made on first use, then reused and kept); without it, a temp folder per run.
import atexit, os, shutil, sys, tempfile

GXAPP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def work():
    w = os.environ.get('XREG_REVIEW_WORK')
    if not w:
        w = tempfile.mkdtemp(prefix='xreg-review-')
        atexit.register(shutil.rmtree, w, True)
    pkg = os.path.join(w, 'out', 'venus-data.zip')
    if not os.path.isfile(pkg):
        skip = shutil.ignore_patterns('__pycache__', '.DS_Store')
        shutil.copytree(GXAPP, os.path.join(w, 'gx_app'), dirs_exist_ok=True,
                        ignore=lambda d, names: set(skip(d, names)) | ({'tests'} if d == GXAPP else set()))
        sys.path.insert(0, os.path.join(w, 'gx_app'))
        import build_package as bp
        os.makedirs(os.path.dirname(pkg), exist_ok=True)
        bp.build_zip(pkg, '0.3', 1761800000)
        bp.check_zip(pkg, '0.3+1761800000')
        sys.path.remove(os.path.join(w, 'gx_app'))
    return w
