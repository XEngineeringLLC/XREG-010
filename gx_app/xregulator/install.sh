#!/bin/sh
# Enables the XREG-010 app: gui-v2 plugin (via /data/apps/enabled) and the bridge service, re-created at every boot.
# The service runs from a copy in /service: daemontools puts root-only supervise/ dirs in it, and dbus-flashmq
# (not root) walks /data/apps/enabled to publish plugins to remote console, failing on any unreadable dir.
APP=/data/apps/available/xregulator
chmod +x $APP/service/run $APP/service/log/run $APP/bridge/xregulator_bridge.py
ln -sfn $APP /data/apps/enabled/xregulator
[ -f /data/rc.local ] || { echo '#!/bin/sh' > /data/rc.local; chmod +x /data/rc.local; }
sed -i '/xregulator/d' /data/rc.local
echo "[ -d /service/xregulator ] || cp -a $APP/service /service/xregulator" >> /data/rc.local
[ -d /service/xregulator ] || cp -a $APP/service /service/xregulator
