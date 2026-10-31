import sys, os, subprocess, glob
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin = sys.argv[1]
stubs = qmlenv.stubs('SevenInch', 'Dark')
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
app = QGuiApplication(sys.argv[:1])
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(plugin).toString())
eng.rootContext().setContextProperty('fileList', sorted(os.path.basename(f) for f in glob.glob(plugin + '/*.qml')))
eng.load(QUrl.fromLocalFile(os.path.join(here, 'compile_all.qml')))
app.exec()
