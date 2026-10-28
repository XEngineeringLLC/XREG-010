#!/bin/zsh
# Installs xregulator/ on the bench GX (GX=<ip> overrides the address): mirrors it into
# /data/apps/available/xregulator, re-runs install.sh, restarts the bridge, and re-creates the app
# link when the plugin changed (gui-v2 rescans on link changes, not on file changes).
set -e
HERE=${0:A:h}
GX=${GX:-172.20.10.2}
SSH="sshpass -p 00000000 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
APP=/data/apps/available/xregulator
# -rlpt, not -a: files must land root-owned on the GX
changed=$(rsync -rlpt --delete --itemize-changes --exclude=__pycache__ --exclude=.DS_Store \
  -e "$SSH" $HERE/xregulator/ root@$GX:$APP/)
print -r -- "$changed"
${=SSH} root@$GX "sh $APP/install.sh && cp $APP/service/run /service/xregulator/run && cp $APP/service/log/run /service/xregulator/log/run && svc -t /service/xregulator"
echo "Bridge restarted."
if [[ $changed == *gui-v2/* ]]; then
  ${=SSH} root@$GX "rm /data/apps/enabled/xregulator && sleep 2 && ln -s $APP /data/apps/enabled/xregulator"
  echo "Plugin changed: app link re-created so gui-v2 reloads it."
fi
