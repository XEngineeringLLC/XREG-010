#!/bin/sh
# Enables the XREG-010 app: gui-v2 plugin (via /data/apps/enabled) and the bridge service, re-created at every boot.
# The service runs from a copy in /service: daemontools puts root-only supervise/ dirs in it, and dbus-flashmq
# (not root) walks /data/apps/enabled to publish plugins to remote console, failing on any unreadable dir.
# Every install path runs this file: venus-data.zip's hooks (a copy in its rc/), the bridge's updater (xreg_update.py)
# and push_to_gx.sh. A release is "<version>+<build>" (the tree's VERSION and BUILD files; no BUILD is build 0),
# ordered by version field by field, then build. Modes:
#   install <dir> [--upgrade-only]  swap the app tree in <dir> for the installed one and start it; the old one is kept
#                                   in $WORK/prev and put back if the new bridge does not start
#   confirm                         started detached by an install made before svscan runs (the stick at boot): waits
#                                   for the new bridge, else puts the old one back
#   boot                            from /data/rc.local: the /service copy, then an install still to confirm
#   admit                           venus-data.zip's pre-hook: exit 0 lets in a newer release, or the installed one
#                                   again when the install is damaged
#   unpacked success|...            venus-data.zip's post-hook
#   remove                          everything this script made, except the bridge's settings in localsettings
#   (none)                          enable the installed tree where it is
# Every install ends with $WORK/result. Nothing waits on the lock while it waits for a bridge, so update-data.sh at boot
# step 30, where no bridge can start until svscan (step 95), never waits more than a few seconds. XREG_DATA,
# XREG_SERVICE and XREG_UPTIME move /data, /service and /proc/uptime (offline tests).

umask 022
DATA=${XREG_DATA:-/data}
SERVICE=${XREG_SERVICE:-/service}
APP=$DATA/apps/available/xregulator
LINK=$DATA/apps/enabled/xregulator
WORK=$DATA/xregulator/install
SVC=$SERVICE/xregulator
RC=$DATA/rc.local
LOGDIR=${XREG_LOGDIR:-/var/log/xregulator}
UPTIME=${XREG_UPTIME:-/proc/uptime}
# Seconds for a new bridge to start. With the GX up (push, in-app) the bench took 9 s. From boot every Venus OS service
# starts at once: the bench Ekrano ran the app about a minute after power-on, and a Cerbo GX is slower. An install begun
# in the first BOOT_UPTIME_S of uptime gets the boot window, svscan running or not: update-data.sh unpacks a slow stick
# after its 30 s wait for /media, while the services are still starting.
VERIFY_S=${XREG_VERIFY_S:-120}
BOOT_VERIFY_S=${XREG_BOOT_VERIFY_S:-600}
BOOT_UPTIME_S=${XREG_BOOT_UPTIME_S:-600}
STABLE_S=${XREG_STABLE_S:-10}
BOOT_ID=${XREG_BOOT_ID:-$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)}
RUN_ID=${XREG_RUN_ID:--}   # the bridge's id for an install it started, repeated in the result
TAG='# xregulator-app'
OLD_LINE="[ -d $SERVICE/xregulator ] || cp -a $APP/service $SERVICE/xregulator"   # what install.sh wrote before 0.3
CR=$(printf '\r')
case $0 in */*) HERE=${0%/*} ;; *) HERE=. ;; esac   # the package's own VERSION, BUILD and FILES sit beside this file
LOCKED=

log() {
	echo "$*"
	[ ! -d "$WORK" ] || echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$WORK/install.log"
}

# for the result's note, which the GX app page shows
note() {
	echo "$*" >> "$WORK/notes"
}

# A notification on the GX screen, for an outcome the app's page cannot show. venus-platform's /Notifications/Inject
# takes "<type>\t<device name>\t<description>", type 2 information. In the background, errors ignored: nothing waits.
notify() {
	command -v dbus-send >/dev/null 2>&1 || return 0
	dbus-send --system --type=method_call --dest=com.victronenergy.platform /Notifications/Inject \
		com.victronenergy.BusItem.SetValue "variant:string:$(printf '2\tXREG-010 GX app\t%s' "$*")" \
		</dev/null >/dev/null 2>&1 &
}

valid_version() {
	case $1 in '' | *[!0-9.]* | .* | *. | *..*) return 1 ;; esac
}

# an app tree's version: its VERSION file; trees from before 0.3 carry it only in the bridge
version_of() {
	if [ -f "$1/VERSION" ]; then
		v=$(head -n 1 "$1/VERSION" | tr -d ' \r\n')
	else
		v=$(sed -n "s/^VERSION = '\([0-9.]*\)'.*/\1/p" "$1/bridge/xreg_core.py" 2>/dev/null | head -n 1)
	fi
	valid_version "$v" && echo "$v"
}

# an app tree's build: 0 without a BUILD file; else its first line must be 0, or 1 to 12 digits with no leading zero
# (xreg_update.BUILD_RE), and anything else makes the tree a release of no build
build_of() {
	[ -f "$1/BUILD" ] || { echo 0; return 0; }
	b=$(head -n 1 "$1/BUILD")
	case $b in 0) ;; '' | *[!0-9]* | 0*) return 1 ;; esac
	[ ${#b} -le 12 ] || return 1
	echo "$b"
}

rid_of() {
	v=$(version_of "$1") && b=$(build_of "$1") || return 1
	echo "$v+$b"
}

installed() {
	[ -d "$APP" ] && rid_of "$APP"
}

# 0 when release $1 is newer than $2: version field by field (0.10 is newer than 0.9), then build
newer() {
	awk -v a="$1" -v b="$2" 'BEGIN { split(a, pa, "+"); split(b, pb, "+")
		na = split(pa[1], x, "."); nb = split(pb[1], y, ".")
		for (i = 1; i <= (na > nb ? na : nb); i++) { if (x[i] + 0 > y[i] + 0) exit 0; if (x[i] + 0 < y[i] + 0) exit 1 }
		exit (pa[2] + 0 > pb[2] + 0) ? 0 : 1 }'
}

# seconds since boot: deadlines never move when NTP or GPS sets the clock of a GX that booted with a stale time
now_s() {
	if read -r up _ 2>/dev/null < "$UPTIME"; then echo "${up%%.*}"; else date +%s; fi
}

svscan_up() {
	pidof svscan >/dev/null 2>&1 || pgrep -x svscan >/dev/null 2>&1
}

# the seconds a new bridge gets to start: the boot window while the GX is still starting its services
window() {
	if ! svscan_up || [ "$(now_s)" -lt "$BOOT_UPTIME_S" ]; then echo "$BOOT_VERIFY_S"; else echo "$VERIFY_S"; fi
}

# 0 when the process named in $WORK/$1 ("pid boot-id": lock/owner, or confirming) runs, this boot
owner_live() {
	read -r pid bid 2>/dev/null < "$WORK/$1" || return 1
	[ -n "$pid" ] && [ "$bid" = "$BOOT_ID" ] && kill -0 "$pid" 2>/dev/null
}

lock_live() {
	[ -d "$WORK/lock" ] && owner_live lock/owner
}

# One install.sh at a time; the lock names its pid and boot id, and one from an earlier boot or a gone process is stale.
# $1: seconds to wait for it.
lock() {
	mkdir -p "$WORK" || return 1
	n=0
	until mkdir "$WORK/lock" 2>/dev/null; do
		n=$((n + 1))
		[ -d "$WORK" ] || return 1   # Remove took the app away while this waited
		# no lock there (a full /data) or one that would not go: wait below, never retry at once
		if [ -d "$WORK/lock" ] && ! owner_live lock/owner; then
			# an owner file not yet written is a lock being taken: a few seconds' grace
			if [ -s "$WORK/lock/owner" ] || [ $n -gt 3 ]; then
				rm -rf "$WORK/lock"
				[ -d "$WORK/lock" ] || continue
			fi
		fi
		[ $n -lt "$1" ] || return 1
		sleep 1
	done
	echo "$$ $BOOT_ID" > "$WORK/lock/owner"
	LOCKED=1
	trap 'rm -rf "$WORK/lock"' EXIT
	trap 'exit 1' INT TERM
	if [ -f "$WORK/install.log" ] && [ $(($(wc -l < "$WORK/install.log"))) -gt 400 ]; then
		tail -n 200 "$WORK/install.log" > "$WORK/install.log.new" && cat "$WORK/install.log.new" > "$WORK/install.log"
		rm -f "$WORK/install.log.new"
	fi
}

unlock() {
	rm -rf "$WORK/lock"
	LOCKED=
}

# "<ok|rolledback|failed> <to> <from or -> <epoch> <run id> <note>", read by the bridge (xreg_update.read_result). The
# holder of the lock adds this install's notes and ends its pending install; without the lock the result is written
# beside an install that is still running, which overwrites it when it ends.
result() {
	n=$4
	if [ -n "$LOCKED" ] && [ -s "$WORK/notes" ]; then
		n="$n $(tr '\n' ' ' < "$WORK/notes")"
	fi
	n=$(echo "$n" | sed 's/^ *//; s/ *$//')
	echo "$1 ${2:-unknown} ${3:--} $(date +%s) $RUN_ID $n" > "$WORK/result.new" && mv "$WORK/result.new" "$WORK/result"
	[ -z "$LOCKED" ] || rm -f "$WORK/pending" "$WORK/notes" "$WORK/confirming"
	log "result: $1 ${2:-unknown} (was ${3:--}) $n"
}

rc_line() {
	echo "[ -d $APP ] || [ ! -d $WORK/prev ] || mv $WORK/prev $APP; [ -f $APP/install.sh ] && sh $APP/install.sh boot >/dev/null 2>&1 & $TAG"
}

rc_strip() {
	grep -v -F -x -e "$OLD_LINE" "$1" | grep -v -e "$TAG\$"
	return 0
}

# lines of ours in a file: the tagged line and the pre-0.3 one
rc_ours() {
	echo $(($(grep -c -e "$TAG\$" "$1") + $(grep -c -F -x -e "$OLD_LINE" "$1")))
}

# the file to rewrite for rc file $1: itself, or the regular file a link points to; nothing for a dangling link
rc_real() {
	if [ -L "$1" ]; then
		t=$(readlink -f "$1" 2>/dev/null) || t=
		if [ -z "$t" ]; then
			# a readlink without -f: one level, which must then be the file itself
			t=$(readlink "$1" 2>/dev/null) || return 1
			case $t in /*) ;; *) t=${1%/*}/$t ;; esac
		fi
		[ -n "$t" ] && [ -f "$t" ] && [ ! -L "$t" ] && echo "$t"
	else
		echo "$1"
	fi
}

# a temp file a power cut left inside rc_commit
rc_tidy() {
	for f in "$RC" "$RC.disabled"; do
		rm -f "$f.xregulator-new"
		t=$(rc_real "$f") && rm -f "$t.xregulator-new"
	done
	return 0
}

# Puts a rebuilt rc file ($2, made from a cp -p of $1, so the mode and owner carry over) in place of $1 once it holds
# $3 owner lines and $4 of ours: a short write (a full /data) never replaces the owner's file. $1 is the file itself,
# never a link to it, and $2 sits beside it, so the rename stays in one directory.
rc_commit() {
	if [ $(grep -c '' "$2") -ne $(($3 + $4)) ] || [ "$(rc_ours "$2")" != "$4" ]; then
		rm -f "$2"
		return 1
	fi
	sync
	mv "$2" "$1"
}

rc_unwritten() {
	note "The startup line could not be written to $1, which was left as it was, so the app will not start after a restart."
}

# The rc.local line that starts the app at boot, right after the shebang (at the top without one), so nothing the
# owner has before it can end the script first (an exec, an exit). With Venus OS's custom startup scripts disabled
# (rc.local renamed rc.local.disabled) the line goes in that file, and the app starts at boot once they are re-enabled.
# A link is kept and the file it points to rewritten. Notes what the owner needs to know. 0 done, 1 done in
# rc.local.disabled, 2 not written (the file left as it was, the reason in the note).
rc_set() {
	f=$RC
	[ -e "$f" ] || [ -L "$f" ] || [ ! -f "$RC.disabled" ] || f=$RC.disabled
	name=/data/${f##*/}
	if ! real=$(rc_real "$f"); then
		note "$name is a link to $(readlink "$f"), which is not a file that can be rewritten, so it was left as it is and the app will not start after a restart."
		return 2
	fi
	new=$real.xregulator-new
	crlf=
	if [ -f "$real" ]; then
		! grep -q "$CR" "$real" || crlf=1
		# CRLF and a lone CR (old Mac files) both end a line
		cp -p "$real" "$new" && sed "s/$CR\$//" "$real" | tr "$CR" '\n' > "$WORK/rc.lf" || { rm -f "$new"; rc_unwritten "$name"; return 2; }
	else
		: > "$WORK/rc.lf" && : > "$new" && chmod 755 "$new" || { rm -f "$new"; rc_unwritten "$name"; return 2; }
		echo "$f" > "$WORK/rc.created"
	fi
	rc_strip "$WORK/rc.lf" > "$WORK/rc.owner"
	owners=$(grep -c '' "$WORK/rc.owner")
	if [ "$owners" -ne $(($(grep -c '' "$WORK/rc.lf") - $(rc_ours "$WORK/rc.lf"))) ]; then
		rm -f "$new"
		rc_unwritten "$name"
		return 2
	fi
	if [ "$owners" = 0 ]; then
		printf '#!/bin/sh\n' > "$WORK/rc.owner"   # an empty file, or one with only our line
		owners=1
	fi
	awk -v line="$(rc_line)" 'NR == 1 && /^#!/ { print; print line; next } NR == 1 { print line } { print }' \
		"$WORK/rc.owner" > "$new" || { rm -f "$new"; rc_unwritten "$name"; return 2; }
	rc_commit "$real" "$new" "$owners" 1 || { rc_unwritten "$name"; return 2; }
	[ -z "$crlf" ] || note "$name had carriage-return line endings (Windows or old Mac), so Venus OS could not run it. It has Unix line endings now, so its other lines run at boot as well."
	if [ "$f" != "$RC" ]; then
		note "Custom startup scripts are disabled on this GX, so the app will not start after a restart until they are re-enabled in Settings > General > Support status."
		return 1
	fi
	if [ ! -x "$real" ]; then
		chmod 755 "$real"
		note "/data/rc.local was not executable, so Venus OS skipped it. It is executable now, so its other lines run at boot as well."
	fi
	return 0
}

# Takes our lines out of an rc file the same way, every owner line kept
rc_drop() {
	real=$(rc_real "$1") || return 1
	new=$real.xregulator-new
	cp -p "$real" "$new" && rc_strip "$real" > "$WORK/rc.owner" && cat "$WORK/rc.owner" > "$new" || { rm -f "$new"; return 1; }
	rc_commit "$real" "$new" $(($(grep -c '' "$real") - $(rc_ours "$real"))) 0
}

# The service copy in /service: Venus OS rebuilds /service from its rootfs at every boot. Built under a dot name, which
# svscan skips, so it never starts half a copy.
service_up() {
	if [ -d "$SVC" ]; then
		for r in run log/run; do
			cp "$APP/service/$r" "$SVC/$r.new" && chmod 755 "$SVC/$r.new" && mv "$SVC/$r.new" "$SVC/$r" || return 1
		done
	else
		rm -rf "$SERVICE/.xregulator.new"
		cp -a "$APP/service" "$SERVICE/.xregulator.new" && mv "$SERVICE/.xregulator.new" "$SVC"
	fi
}

# gui-v2 reloads plugins when a link in /data/apps/enabled changes, not when a file under it does: the link is made again
# when the plugin differs from the one it replaces ($1; '' for none)
plugin_link() {
	mkdir -p "$DATA/apps/enabled"
	if [ -L "$LINK" ] && [ "$(readlink "$LINK")" = "$APP" ] && [ -n "$1" ] && cmp -s "$1" "$APP/gui-v2/xregulator.json"; then
		return 0
	fi
	if [ -e "$LINK" ] && [ ! -L "$LINK" ]; then
		log "$LINK is not a link; plugin left as it is"
		return 1
	fi
	if [ -L "$LINK" ]; then
		rm -f "$LINK"
		sleep 2
	fi
	ln -s "$APP" "$LINK"
}

# Readable by everyone (dbus-flashmq walks the app without root), and the executables
tidy() {
	chmod -R u+rwX,go+rX,go-w "$1"
	chmod 755 "$1/install.sh" "$1/service/run" "$1/service/log/run" "$1/bridge/xregulator_bridge.py"
}

# power lost between the two renames of a swap leaves the previous app whole in prev
restore_prev() {
	[ -d "$APP" ] || [ ! -d "$WORK/prev" ] || { mv "$WORK/prev" "$APP" && log "previous app put back after an interrupted swap"; }
}

# The app, its /service copy, its plugin link and its rc.local line, as Remove takes them out. With "all" also
# /data/xregulator and the bridge's log; else the install's result, log and notes, the failed release and the bridge's
# log stay, to be read later.
take_out() {
	if [ -d "$SVC" ]; then
		# renamed first: svscan skips dot names, so it cannot start supervise again in the dir being removed
		gone=$SERVICE/.xregulator.removed
		rm -rf "$gone"
		mv "$SVC" "$gone"
		svc -dx "$gone" "$gone/log" 2>/dev/null
		n=0
		while { svok "$gone" || svok "$gone/log"; } 2>/dev/null && [ $n -lt 10 ]; do
			sleep 1
			n=$((n + 1))
		done
		rm -rf "$gone"
	fi
	[ ! -L "$LINK" ] || rm -f "$LINK"
	# the previous app first: the rc.local line puts prev back when the app is missing
	rm -rf "$WORK/prev" "$WORK/new" "$APP"
	# the line last: power lost before this leaves a line that finds no app and does nothing
	rc_tidy
	created=$(cat "$WORK/rc.created" 2>/dev/null)
	for f in "$RC" "$RC.disabled"; do
		[ -f "$f" ] || continue
		rc_drop "$f" || log "could not take the startup line out of $f; it finds no app and does nothing"
		# one this script made with nothing else in it goes too: Venus OS lists any rc.local as a modification
		if [ "$created" = "$f" ] && [ ! -L "$f" ] && ! grep -q -v -e '^[[:space:]]*$' -e '^[[:space:]]*#' "$f"; then
			rm -f "$f"
		fi
	done
	if [ "$1" = all ]; then
		rm -rf "$LOGDIR" "$DATA/xregulator"
		return 0
	fi
	for x in "$DATA"/xregulator/* "$WORK"/*; do
		case $x in
		"$WORK" | "$WORK"/result | "$WORK"/install.log | "$WORK"/notes | "$WORK"/failed_version | "$WORK"/lock) ;;
		*) rm -rf "$x" ;;
		esac
	done
}

# 0 when the installed app is whole: its startup line in the file Venus OS runs (rc.local, executable and with a Unix
# first line, when it exists; else rc.local.disabled, which Venus OS renames back only while no rc.local exists), its
# plugin link, and every file as packed (FILES, the package's sha256 list; a tree without one counts as damaged)
healthy() {
	if [ -e "$RC" ] || [ -L "$RC" ]; then
		[ -x "$RC" ] && ! head -n 1 "$RC" | grep -q "$CR" || return 1
		f=$RC
	else
		f=$RC.disabled
	fi
	grep -q -e "$TAG\$" "$f" 2>/dev/null || return 1
	[ -L "$LINK" ] && [ "$(readlink "$LINK")" = "$APP" ] || return 1
	[ -f "$APP/FILES" ] && cmp -s "$HERE/FILES" "$APP/FILES" || return 1
	mkdir -p "$WORK" || return 1
	# busybox has sha256sum on Venus OS v3.85 (Yocto scarthgap); later Yocto busybox configs leave it out
	if command -v sha256sum >/dev/null 2>&1; then
		(cd "$APP" && awk '{ print $2 }' FILES | xargs sha256sum) > "$WORK/files.now" 2>/dev/null
	else
		(cd "$APP" && python3 -c 'import hashlib, sys
for p in sys.argv[1:]:
    print("%s  %s" % (hashlib.sha256(open(p, "rb").read()).hexdigest(), p))' $(awk '{ print $2 }' FILES)) \
			> "$WORK/files.now" 2>/dev/null
	fi
	cmp -s "$WORK/files.now" "$APP/FILES"
}

# 0 once the bridge of release $1 has written $WORK/running this boot (xreg_update.mark_running) and is still the same
# process STABLE_S later, within $2 seconds. A bridge started before the swap is restarted once. With a token ($3), 2 as
# soon as $WORK/pending no longer carries it: another install has taken over.
verify() {
	end=$(($(now_s) + $2))
	kicked=
	while [ "$(now_s)" -lt "$end" ]; do
		if [ -n "$3" ]; then
			k=
			read -r _ _ _ _ k 2>/dev/null < "$WORK/pending"
			[ "$k" = "$3" ] || return 2
		fi
		if read -r v pid bid 2>/dev/null < "$WORK/running" && [ "$bid" = "$BOOT_ID" ] && kill -0 "$pid" 2>/dev/null; then
			if [ "$v" = "$1" ]; then
				sleep "$STABLE_S"
				read -r v2 pid2 bid2 2>/dev/null < "$WORK/running"
				[ "$v2 $pid2" = "$v $pid" ] && kill -0 "$pid" 2>/dev/null && return 0
			elif [ -z "$kicked" ] && svok "$SVC" 2>/dev/null; then
				log "bridge $v still running; restarting it"
				svc -t "$SVC"
				kicked=1
			fi
		fi
		sleep 1
	done
	return 1
}

# Puts the previous release back after $1 failed to start; $2 is that release, $3 the reason, $4 the seconds to wait for
# it. With no previous release the failed one is taken out, so the GX is as before the install and a newer stick can
# go in; its result and log stay, and the reason goes to the GX's notifications. Holds the lock.
rollback() {
	echo "$1" > "$WORK/failed_version"
	if [ ! -d "$WORK/prev" ]; then
		log "no previous release: taking $1 out again"
		take_out keep
		why="$3 There was no previous version, so the app was removed again."
		result failed "$1" "" "$why"
		notify "$why"
		return 1
	fi
	log "putting ${2:-the previous version} back: $3"
	rm -rf "$WORK/failed"
	if ! mv "$APP" "$WORK/failed" || ! mv "$WORK/prev" "$APP"; then
		restore_prev
		result failed "$1" "$2" "$3 The previous version could not be put back."
		return 1
	fi
	service_up
	plugin_link "$WORK/failed/gui-v2/xregulator.json"
	rm -f "$WORK/running"
	svok "$SVC" 2>/dev/null && svc -t "$SVC"
	if [ ! -f "$APP/bridge/xreg_update.py" ]; then
		result rolledback "$1" "$2" "$3 The previous version is back."   # bridges before 0.3 write no running marker
	elif verify "$2" "$4"; then
		result rolledback "$1" "$2" "$3 The previous version is back and running."
	else
		result rolledback "$1" "$2" "$3 The previous version is back; it had not started within $4 s when this was written."
	fi
	rm -rf "$WORK/failed"
	sync
	return 1
}

# Waits, without the lock, up to $1 seconds for the bridge of the release $WORK/pending names, then takes the lock for
# the verdict, or to put the previous release back. An install that replaces the pending one meanwhile (a second
# archive on the stick) takes over, and this wait ends without a verdict.
wait_confirm() {
	read -r to from bid id token 2>/dev/null < "$WORK/pending" || return 0
	echo "$$ $BOOT_ID" > "$WORK/confirming"
	verify "$to" "$1" "$token"
	ok=$?
	if ! lock 600; then
		log "release $to: the lock stayed taken past 600 s, no verdict written"
		return 1
	fi
	if ! read -r _ _ _ _ k 2>/dev/null < "$WORK/pending" || [ "$k" != "$token" ]; then
		[ "$(cat "$WORK/confirming" 2>/dev/null)" != "$$ $BOOT_ID" ] || rm -f "$WORK/confirming"
		log "release $to: another install took over"
		unlock
		return 0
	fi
	[ "$from" = - ] && from=
	RUN_ID=${id:--}
	if [ "$(installed)" != "$to" ]; then
		result failed "$to" "$from" "The install was interrupted; the previous version is in place."
		return 1
	fi
	if [ $ok = 0 ]; then
		result ok "$to" "$from" ""
		rm -f "$WORK/failed_version"
		sync
		return 0
	fi
	rollback "$to" "$from" "The new version did not start within $1 s." "$1"
}

# A detached install.sh confirm, for an install made before svscan runs (the stick at boot): this one returns at once
# so the boot goes on, and the confirmation never depends on rc.local running later
start_confirmer() {
	if command -v setsid >/dev/null 2>&1; then
		setsid sh "$APP/install.sh" confirm </dev/null >/dev/null 2>&1 &
	else
		sh "$APP/install.sh" confirm </dev/null >/dev/null 2>&1 &
	fi
}

# an install from earlier that never got its verdict (the power went while it waited) and nothing now waits for
unclaimed() {
	[ -f "$WORK/pending" ] && ! lock_live && ! owner_live confirming
}

do_install() {
	src=$1
	wait_s=600
	svscan_up || wait_s=10   # boot step 30: nothing there may wait long
	if ! lock "$wait_s"; then
		result failed "$(rid_of "$src")" "$(installed)" "Another install did not finish within $wait_s s, so this one did not run."
		exit 1
	fi
	trap '' HUP PIPE   # a bench SSH session ending must not stop an install halfway
	restore_prev
	rc_tidy
	rm -f "$WORK/notes"
	from=$(installed)
	if [ ! -d "$src" ] || [ "$(cd "$src" && pwd -P)" = "$(cd "$APP" 2>/dev/null && pwd -P)" ]; then
		result failed unknown "$from" "The new app files were not found."
		exit 1
	fi
	to=$(rid_of "$src")
	for f in VERSION install.sh bridge/xregulator_bridge.py bridge/xreg_core.py bridge/xreg_update.py \
		gui-v2/xregulator.json service/run service/log/run; do
		if [ ! -f "$src/$f" ] || [ -z "$to" ]; then
			result failed "$to" "$from" "The package is incomplete or damaged ($f)."
			rm -rf "$src"
			exit 1
		fi
	done
	if [ "$2" = --upgrade-only ] && [ -n "$from" ] && ! newer "$to" "$from"; then
		result failed "$to" "$from" "It is not newer than the installed version."
		rm -rf "$src"
		exit 1
	fi
	tidy "$src"
	log "installing $to over ${from:-nothing}"
	rm -rf "$WORK/prev"
	echo "$to ${from:--} $BOOT_ID $RUN_ID $$.$(now_s)" > "$WORK/pending"
	if [ -d "$APP" ] && ! mv "$APP" "$WORK/prev"; then
		result failed "$to" "$from" "The installed version could not be moved aside."
		exit 1
	fi
	mkdir -p "$DATA/apps/available"
	if ! mv "$src" "$APP"; then
		restore_prev
		result failed "$to" "$from" "The new version could not be moved in."
		exit 1
	fi
	rc_set
	service_up || log "could not refresh $SVC"
	plugin_link "${from:+$WORK/prev/gui-v2/xregulator.json}"
	sync
	win=$(window)
	if svscan_up; then
		rm -f "$WORK/running"
		svok "$SVC" 2>/dev/null && svc -t "$SVC"
		echo "$$ $BOOT_ID" > "$WORK/confirming"   # before the unlock, so no rc.local boot step takes this install as unclaimed
		unlock
		wait_confirm "$win"
		exit $?
	fi
	unlock
	start_confirmer
	log "svscan is not running yet: a detached install.sh confirm waits up to $win s for release $to"
	exit 0
}

do_confirm() {
	mkdir -p "$WORK" && exec 2>>"$WORK/install.log"
	[ -f "$WORK/pending" ] || exit 0
	# what boot does: an install resumed by the stick at a later boot finds no service copy and maybe no link
	if lock 10; then
		restore_prev
		rc_tidy
		service_up || log "could not make $SVC"
		[ -L "$LINK" ] || plugin_link ""
		unlock
	fi
	wait_confirm "$BOOT_VERIFY_S"
}

do_boot() {
	mkdir -p "$WORK" && exec 2>>"$WORK/install.log"
	lock 60 || exit 0
	restore_prev
	rc_tidy
	service_up || log "could not make $SVC"
	[ -L "$LINK" ] || plugin_link ""
	unlock
	if unclaimed; then
		wait_confirm "$BOOT_VERIFY_S"
	fi
}

do_admit() {
	restore_prev
	pkg=$(rid_of "$HERE")
	from=$(installed)
	if [ -z "$pkg" ]; then
		log "XREG-010 GX app: package version not readable; skipped"
		return 1
	fi
	# a release that failed to start is not tried again from a stick, unless it is the installed one, to repair
	if [ -n "$from" ] && [ "$pkg" = "$from" ]; then
		if ! healthy; then
			rm -rf "$WORK/new"
			log "XREG-010 GX app: $from is damaged; USB/SD package $pkg admitted to repair it"
			return 0
		fi
		log "XREG-010 GX app: $from installed; USB/SD package $pkg skipped"
	elif [ "$(cat "$WORK/failed_version" 2>/dev/null)" = "$pkg" ]; then
		log "XREG-010 GX app: $pkg did not start before; USB/SD package skipped"
	elif [ -z "$from" ] || newer "$pkg" "$from"; then
		rm -rf "$WORK/new"
		log "XREG-010 GX app: USB/SD package $pkg admitted over ${from:-nothing}"
		return 0
	else
		log "XREG-010 GX app: $from installed; USB/SD package $pkg skipped"
	fi
	if unclaimed; then
		read -r to from bid id 2>/dev/null < "$WORK/pending"
		[ "$bid" = "$BOOT_ID" ] || start_confirmer
	fi
	return 1
}

do_unpacked() {
	[ "$1" != success ] || [ ! -d "$WORK/new" ] || do_install "$WORK/new"
	mkdir -p "$WORK"
	rm -rf "$WORK/new"
	result failed "$(rid_of "$HERE")" "$(installed)" "The USB/SD package did not unpack ($1)."
	exit 1
}

do_remove() {
	# the bridge runs this from a private copy in /tmp, which goes too
	case ${0##*/} in xregulator-remove-*) copy=$0 ;; *) copy= ;; esac
	if ! lock 600; then
		result failed "$(installed)" "$(installed)" "Remove did not run: another install did not finish within 600 s."
		[ -z "$copy" ] || rm -f "$copy"
		exit 1
	fi
	trap 'rm -rf "$WORK/lock"; [ -z "$copy" ] || rm -f "$copy"' EXIT
	trap '' HUP PIPE
	log "removing the app"
	take_out all
	sync
	log "XREG-010 GX app removed"
}

do_enable() {
	lock 600 || exit 1
	restore_prev
	rc_tidy
	[ -d "$APP" ] || { log "no app at $APP"; exit 1; }
	rm -f "$WORK/notes"
	tidy "$APP"
	rc_set
	service_up
	plugin_link "$APP/gui-v2/xregulator.json"
	svok "$SVC" 2>/dev/null && svc -t "$SVC"
	return 0
}

case ${1:-enable} in
install)
	[ -n "$2" ] || { echo "usage: $0 install <app tree> [--upgrade-only]"; exit 2; }
	do_install "$2" "$3" ;;
confirm) do_confirm ;;
boot) do_boot ;;
admit) do_admit ;;
unpacked) do_unpacked "$2" ;;
remove) do_remove ;;
enable) do_enable ;;
*) echo "usage: $0 [install <app tree> [--upgrade-only] | confirm | boot | admit | unpacked <result> | remove]"; exit 2 ;;
esac
