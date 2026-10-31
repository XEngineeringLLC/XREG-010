#!/bin/sh
# Venus OS's update-data.sh runs this after it unpacks venus-data.zip into /data, with success or extraction-failed.
exec sh "${0%/*}/install.sh" unpacked "$1"
