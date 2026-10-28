#!/bin/zsh
# Builds the gui-v2 plugin xregulator/gui-v2/xregulator.json from the QML in plugin/.
# Needs PySide6 for rcc, lupdate and lrelease: python3 -m pip install --break-system-packages PySide6
set -e
HERE=${0:A:h}
VERSION=0.1
PYS=$(python3 -c 'import PySide6, os; print(os.path.dirname(PySide6.__file__))')
WORK=$(mktemp -d)
trap 'rm -rf $WORK' EXIT
mkdir $WORK/bin $WORK/xregulator   # the compiler requires its working folder to carry the plugin name
ln -s $PYS/lupdate $PYS/lrelease $WORK/bin/
# The stock compiler leaves rcc at its 70% threshold, which stores most QML raw: about 2.4x the size.
# The plugin reaches the browser remote console over MQTT in 4 KB chunks, so every byte counts.
print -r -- "#!/bin/sh
exec $PYS/Qt/libexec/rcc -compress 9 -threshold 0 \"\$@\"" > $WORK/bin/rcc
chmod +x $WORK/bin/rcc
cp $HERE/plugin/*.qml $WORK/xregulator/
cd $WORK/xregulator
PATH=$WORK/bin:$PATH python3 $HERE/tools/victron/gui-v2-plugin-compiler.py \
  --name xregulator --version $VERSION --min-required-version v1.2.13 \
  --settings XregSettings.qml \
  --devicelist 0xB091 XregAlternator.qml Dashboard
cp xregulator.json $HERE/xregulator/gui-v2/xregulator.json
echo "Built $HERE/xregulator/gui-v2/xregulator.json"
