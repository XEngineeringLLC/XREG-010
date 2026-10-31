#!/bin/zsh
# Downloads what the offline suites use but the repo does not keep, pinned and checked, into a cache outside the repo:
#   gui-v2 1.3.22 source, the gui-v2 on the bench Ekrano (QML suites: theme JSON, Roboto font, and ListSwitch.qml and
#     two images of the stub module); 5.4 MB tarball, 24 MB unpacked
#   Victron's update-data.sh as in meta-victronenergy v3.85 (test_real_update_data.py), 1.9 KB
# Prints the cache folder. XREG_TEST_CACHE moves it.
set -e
CACHE=${XREG_TEST_CACHE:-$HOME/Library/Caches/xreg-gx-tests}
GUIV2_URL=https://github.com/victronenergy/gui-v2/archive/refs/tags/v1.3.22.tar.gz
GUIV2_SHA=702ece821e6b705d1e9101dba956c487b9558c9dfd2bb0859ff29250fc018f9f
# v3.85 is commit c38e12a1 of meta-victronenergy
UPD_URL=https://raw.githubusercontent.com/victronenergy/meta-victronenergy/c38e12a1851ca2196a8e1d0f8e96d23891e91f1b/meta-venus/recipes-core/initscripts/files/update-data.sh
UPD_SHA=98a15d06ec4d10d6974cd72e5abfcd353e0b495b07205a9b0483dec8626d972e
mkdir -p $CACHE

fetch() {   # fetch <url> <sha256> <file>
  [[ -f $3 && $(shasum -a 256 $3 | cut -d' ' -f1) == $2 ]] && return 0
  local tmp=$(mktemp -d $CACHE/dl.XXXXXX)
  curl -sSfL -o $tmp/f $1
  local got=$(shasum -a 256 $tmp/f | cut -d' ' -f1)
  if [[ $got != $2 ]]; then
    rm -rf $tmp
    print -u2 "$1: sha256 $got, expected $2"
    exit 1
  fi
  mv $tmp/f $3
  rmdir $tmp
}

fetch $GUIV2_URL $GUIV2_SHA $CACHE/gui-v2-1.3.22.tar.gz
if [[ ! -f $CACHE/gui-v2-1.3.22/.complete ]]; then
  rm -rf $CACHE/gui-v2-1.3.22 $CACHE/unpack
  mkdir $CACHE/unpack
  tar xzf $CACHE/gui-v2-1.3.22.tar.gz -C $CACHE/unpack
  mv $CACHE/unpack/gui-v2-1.3.22 $CACHE/gui-v2-1.3.22
  rmdir $CACHE/unpack
  touch $CACHE/gui-v2-1.3.22/.complete
fi
fetch $UPD_URL $UPD_SHA $CACHE/update-data_v3.85.sh
print -r -- $CACHE
