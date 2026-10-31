#!/bin/sh
# Venus OS's update-data.sh runs this at every boot with venus-data.zip on a USB stick or SD card, before it unpacks the
# zip into /data; a non-zero exit skips the unpack. install.sh (a copy beside this file) lets only a newer version in.
exec sh "${0%/*}/install.sh" admit
