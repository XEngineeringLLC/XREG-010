#!/bin/zsh
# Installs xregulator/ on the bench GX (GX=<ip> overrides the address) through install.sh, as the USB/SD package and the
# in-app updater do: mirrors it into the stage /data/xregulator/install/new (unchanged files copied on the GX from the
# installed app), then the staged install.sh swaps it in, restarts the bridge, re-creates the app link when the plugin
# changed (gui-v2 rescans on link changes, not on file changes), and puts the previous version back if the new bridge
# does not start.
set -e
HERE=${0:A:h}
GX=${GX:-172.20.10.2}
SSH="sshpass -p 00000000 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
APP=/data/apps/available/xregulator
NEW=/data/xregulator/install/new
${=SSH} root@$GX "mkdir -p $NEW"
# -rlpt, not -a: files must land root-owned on the GX
rsync -rlpt --delete --copy-dest=$APP --exclude=__pycache__ --exclude=.DS_Store -e "$SSH" $HERE/xregulator/ root@$GX:$NEW/
# a bench bridge that dies does so within seconds: a shorter stability wait than an owner's update
${=SSH} root@$GX "XREG_STABLE_S=3 sh $NEW/install.sh install $NEW"
