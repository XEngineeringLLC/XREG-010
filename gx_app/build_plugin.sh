#!/bin/zsh
# Builds the gui-v2 plugin xregulator/gui-v2/xregulator.json from the QML in plugin/, versioned from xregulator/VERSION.
# Needs PySide6 for rcc, lupdate and lrelease: python3 -m pip install --break-system-packages PySide6
set -e
HERE=${0:A:h}
VERSION=$(<$HERE/xregulator/VERSION)   # the one GX app number: the bridge, build_package.py and install.sh read it too
PYS=$(python3 -c 'import PySide6, os; print(os.path.dirname(PySide6.__file__))')
# gui-v2 shows the plugin name as its Settings > Integrations > UI Plugins row and that page's title. It is also the
# resource prefix every qrc:/XREG-010/ link in plugin/ uses, and the start of its translation file's name.
NAME=XREG-010
WORK=$(mktemp -d)
trap 'rm -rf $WORK' EXIT
mkdir $WORK/bin $WORK/$NAME   # the compiler requires its working folder to carry the plugin name
ln -s $PYS/lupdate $PYS/lrelease $WORK/bin/
# The stock compiler leaves rcc at its 70% threshold, which stores most QML raw: about 2.4x the size.
# The plugin reaches the browser remote console over MQTT in 4 KB chunks, so every byte counts.
print -r -- "#!/bin/sh
exec $PYS/Qt/libexec/rcc -compress 9 -threshold 0 \"\$@\"" > $WORK/bin/rcc
chmod +x $WORK/bin/rcc
cp $HERE/plugin/*.qml $WORK/$NAME/
cd $WORK/$NAME
PATH=$WORK/bin:$PATH python3 $HERE/tools/victron/gui-v2-plugin-compiler.py \
  --name $NAME --version $VERSION --min-required-version v1.3.4 \
  --settings XregSettings.qml \
  --devicelist 0xB091 XregAlternator.qml Dashboard
# the file keeps its old name: install.sh, build_package.py and the updater find the plugin by it, gui-v2 reads any *.json
cp $NAME.json $HERE/xregulator/gui-v2/xregulator.json
echo "Built $HERE/xregulator/gui-v2/xregulator.json"
