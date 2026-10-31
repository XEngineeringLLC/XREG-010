#!/bin/bash
# Publishes the GX app at the version in gx_app/xregulator/VERSION to the public Supabase Storage bucket "ota", the way
# build_and_deploy.sh publishes firmware: build_package.py builds, checks (tools/sim_install.py) and signs the package
# with the firmware OTA key and stamps it with a build, then gxapp_<version>_<sha12>.zip and .sig and venus-data.zip
# (the docs site's download) go up, and gxapp_versions.json last, so no GX is offered a release before its files are
# reachable. Publishing a version number again is a reissue: its manifest entry gets the new build and the new file
# names, which every GX on that version takes as newer. The zip and .sig are named by content and never overwritten,
# because ota.xengineering.net lets caches keep anything but .json for an hour; it forwards single path elements only
# ([A-Za-z0-9._-]), hence flat names, no gx/ folder. venus-data.zip keeps its name for the docs link, so a browser can
# be handed the previous release for up to that hour after a publish.
# Once the manifest is up, the release record, as publish_ota.sh does for firmware; each step is non-fatal, since the
# release is already out: gx_changelog.sh writes the changelog entry, xregbackup2 snapshots the source as
# PUBLISHED_gx<release>_<date> (the commit the next version's changelog diffs from), and the published files and the
# entry go into that snapshot's gx_release/.
# Needs: supabase CLI logged in, openssl, $PRIVATE_KEY, a plugin built at this version (build_plugin.sh); for the
# record, the claude CLI and xregbackup2 in ~/.zshrc.
# Usage: gx_app/publish_gx.sh ["notes"]
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PRIVATE_KEY="/Users/joeceo/private_key.pem"
SUPABASE_WORKDIR="/Users/joeceo/Projects/Cloud Workspace"   # storage commands run from the supabase-linked dir
OTA_BUCKET="ota"
SUPABASE_PUBLIC_BASE="https://qnbekuaoweuteylitzvo.supabase.co/storage/v1/object/public/${OTA_BUCKET}"
OTA_PUBLIC="https://ota.xengineering.net"   # what the bridge and the docs link read

VERSION="$(tr -d ' \r\n' < "$HERE/xregulator/VERSION")"
NOTES="${1:-GX app $VERSION}"
OUT="$(mktemp -d -t gxapp)"

# supa_put <local path> <dest object> [cache-control], as in build_and_deploy.sh: storage cp 409s on an existing
# object, so rm then cp, with one retry
supa_put() {
    local cc="${3:-max-age=3600}"
    (
        cd "$SUPABASE_WORKDIR" || return 1
        supabase storage rm "ss:///${OTA_BUCKET}/$2" --experimental --yes >/dev/null 2>&1 || true
        if ! supabase storage cp "$1" "ss:///${OTA_BUCKET}/$2" --experimental --cache-control "$cc"; then
            echo "   upload of $2 failed, retrying once"
            if ! supabase storage cp "$1" "ss:///${OTA_BUCKET}/$2" --experimental --cache-control "$cc"; then
                echo "   upload of $2 failed after retry: the old copy was already deleted, so '$2' is MISSING"
                return 1
            fi
        fi
    )
}

echo "=== GX app $VERSION: build, sign, publish ==="
[ -f "$PRIVATE_KEY" ] || { echo "Private key not found at $PRIVATE_KEY"; exit 1; }
command -v supabase >/dev/null || { echo "Supabase CLI not found (brew install supabase/tap/supabase)"; exit 1; }
command -v openssl >/dev/null || { echo "openssl not found"; exit 1; }
( cd "$SUPABASE_WORKDIR" && supabase projects list >/dev/null 2>&1 ) || { echo "Supabase CLI not logged in: supabase login"; exit 1; }

# A failed fetch must never rebuild the manifest from empty: that drops every earlier release
MANIFEST="$OUT/base_versions.json"
HTTP=$(curl -s --retry 3 --max-time 15 -w '%{http_code}' "$SUPABASE_PUBLIC_BASE/gxapp_versions.json" -o "$MANIFEST" || echo 000)
BASE_ARG=()
if [ "$HTTP" = "200" ]; then
    BASE_ARG=(--base-manifest "$MANIFEST")
elif [ "$HTTP" = "404" ] || [ "$HTTP" = "400" ]; then
    echo "No gxapp_versions.json yet: this is the first GX app release"
else
    echo "gxapp_versions.json fetch failed (HTTP $HTTP): nothing built or uploaded"
    exit 1
fi

# builds, runs the install simulation, signs, checks the signature with the bridge's own code and key, writes the manifest
python3 "$HERE/build_package.py" "$OUT" --key "$PRIVATE_KEY" --ota-key "${BASE_ARG[@]}" --notes "$NOTES"

ZIP=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["zip"])' "$OUT/release.json")
SIG=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["sig"])' "$OUT/release.json")
REL=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["release"])' "$OUT/release.json")
LOCAL_SHA=$(shasum -a 256 "$OUT/$ZIP" | awk '{print $1}')
echo "Release $REL: $ZIP, $SIG"
echo "Uploading $ZIP, $SIG and venus-data.zip..."
supa_put "$OUT/$ZIP" "$ZIP"
supa_put "$OUT/$SIG" "$SIG"
supa_put "$OUT/venus-data.zip" "venus-data.zip" "max-age=300"   # the docs link: always the latest release

# Through the proxy GXs and browsers use, before the manifest offers the version
echo "Checking the files through $OTA_PUBLIC..."
for OBJ in "$ZIP" "venus-data.zip"; do
    GOT=$(curl -s --max-time 30 "$OTA_PUBLIC/$OBJ" | shasum -a 256 | awk '{print $1}')
    if [ "$GOT" != "$LOCAL_SHA" ]; then
        # venus-data.zip may still be the edge's cached previous release for up to 5 minutes
        if [ "$OBJ" = "venus-data.zip" ]; then
            echo "   venus-data.zip through $OTA_PUBLIC is not this build yet (edge cache, up to 5 min); continuing"
        else
            echo "   $OBJ through $OTA_PUBLIC does not match the local build: manifest NOT updated"
            exit 1
        fi
    fi
done
CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 15 "$OTA_PUBLIC/$SIG" || true)
[ "$CODE" = "200" ] || { echo "   $SIG through $OTA_PUBLIC: HTTP $CODE; manifest NOT updated"; exit 1; }

if ! supa_put "$OUT/gxapp_versions.json" "gxapp_versions.json" "max-age=60"; then
    echo "gxapp_versions.json upload failed and the old copy was already deleted. The full manifest is at:"
    echo "  $OUT/gxapp_versions.json"
    echo "Restore it first: (cd \"$SUPABASE_WORKDIR\" && supabase storage cp \"$OUT/gxapp_versions.json\" ss:///${OTA_BUCKET}/gxapp_versions.json --experimental --cache-control max-age=60)"
    exit 1
fi

LISTED=0
for TRY in 1 2 3 4 5 6 7 8; do
    if curl -s --max-time 15 "$OTA_PUBLIC/gxapp_versions.json?r=$RANDOM" \
        | VERSION="$VERSION" ZIP="$ZIP" python3 -c 'import json, os, sys; e = json.load(sys.stdin)["versions"].get(os.environ["VERSION"], {}); sys.exit(0 if e.get("file") == os.environ["ZIP"] else 1)' 2>/dev/null; then
        LISTED=1
        break
    fi
    [ "$TRY" -lt 8 ] && { echo "   gxapp_versions.json does not list $REL yet ($TRY/8), retrying in 10 s"; sleep 10; }
done
if [ "$LISTED" = 1 ]; then
    echo "GX app $REL published: GXs offer it on the update page; $OTA_PUBLIC/venus-data.zip is this release"
else
    echo "gxapp_versions.json uploaded, but $OTA_PUBLIC did not show $REL within 70 s (edge cache). Check with:"
    echo "  curl -s $OTA_PUBLIC/gxapp_versions.json"
fi

# The release record. Nothing past this point may fail the publish: each step reports and the summary says how to rerun.
RECORD="$OUT/gx_release"
REC_OK=0
mkdir -p "$RECORD" && cp "$OUT/$ZIP" "$OUT/$SIG" "$OUT/venus-data.zip" "$OUT/release.json" "$OUT/gxapp_versions.json" \
    "$RECORD/" && REC_OK=1

echo ""
echo "=== Changelog entry ==="
CL_ENTRY="$RECORD/changelog_entry.md"
"$HERE/gx_changelog.sh" --manifest "$OUT/gxapp_versions.json" --entry "$CL_ENTRY" 2>&1 \
    | tee "$OUT/changelog.log"
CL_RC=${PIPESTATUS[0]}

SNAP_NAME="PUBLISHED_gx${REL}_$(date +%Y-%m-%d)"
# xregbackup2's folder ($HOME is /Users/joeceo there); a test gives HOME a stand-in .zshrc that defines xregbackup2
SNAP="$HOME/Projects/Old Backups 2/Xregulator_$SNAP_NAME"
echo ""
echo "=== Source snapshot $SNAP_NAME ==="
# xregbackup2 is a zsh function, so bash runs it in zsh with ~/.zshrc sourced, as publish_ota.sh gets it
BK_RC=0
zsh -c 'source ~/.zshrc >/dev/null 2>&1; xregbackup2 "$1"' xregbackup2 "$SNAP_NAME" || BK_RC=$?

# After xregbackup2, which rm -rf's its folder first. A failed run that still made the folder (a push failure) keeps it.
RC_OK=0
[ "$REC_OK" = 1 ] && [ -d "$SNAP" ] && cp -R "$RECORD" "$SNAP/" && RC_OK=1

CL_DEST="$CL_ENTRY"
[ "$RC_OK" = 1 ] && CL_DEST="$SNAP/gx_release/changelog_entry.md"
if [ "$CL_RC" = 0 ]; then
    CL_MSG="changelog: $(grep '^Changelog: ' "$OUT/changelog.log" | tail -1 | sed 's/^Changelog: //')"
    CL_MSG="${CL_MSG//"$CL_ENTRY"/$CL_DEST}"
else
    CL_MSG="WARNING: changelog entry FAILED (exit $CL_RC) -- run manually: \"$HERE/gx_changelog.sh\" --entry \"$CL_DEST\""
fi
if [ "$BK_RC" = 0 ]; then
    BK_MSG="local snapshot: $SNAP_NAME"
else
    BK_MSG="WARNING: xregbackup2 snapshot FAILED (exit $BK_RC) -- run manually: xregbackup2 \"$SNAP_NAME\" && cp -R \"$RECORD\" \"$SNAP/\""
fi
if [ "$RC_OK" = 1 ]; then
    RC_MSG="release record: $SNAP/gx_release/ ($(cd "$SNAP/gx_release" && ls | tr '\n' ' ' | sed 's/ $//'))"
elif [ "$REC_OK" = 1 ]; then
    RC_MSG="WARNING: release record NOT written (no snapshot folder) -- the files wait in $RECORD"
else
    RC_MSG="WARNING: release record NOT assembled -- the published files are in $OUT"
fi

echo ""
echo "=== DONE -- GX app $REL is published ==="
echo "$CL_MSG"
echo "$BK_MSG"
echo "$RC_MSG"
echo "Local copies: $OUT"
