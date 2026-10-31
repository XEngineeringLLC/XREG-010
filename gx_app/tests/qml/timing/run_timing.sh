#!/bin/zsh
# Mac timing of two plugin folders, relative numbers only (Metal through QQuickRenderControl, no image read back):
# Short Term redraws (timing.py), page creation (create_timing.py) and the speed-curve sweep (perf_timing.py), twice
# each, on copies with a created-delegate counter (make_variants.py). Prints the results file (JSON lines).
#   run_timing.sh <before plugin dir> <after plugin dir>
# Not part of run_all.sh. The other scripts here (dash_, switch_, slice_timing.py, slice_dist.py, mem_check.py) run
# one at a time, each with its own arguments in its first lines.
set -e
HERE=${0:A:h}
export XREG_TEST_WORK=${XREG_TEST_WORK:-$(mktemp -d -t xreg-timing)}
export PYTHONDONTWRITEBYTECODE=1 QML_DISABLE_DISK_CACHE=1
O=$XREG_TEST_WORK/timing.jsonl
: > $O
cd $HERE
python3 make_variants.py ${1:A} ${2:A}
for rep in 1 2; do
for v in before after; do
  for w in 30 300; do python3 timing.py $XREG_TEST_WORK/variants/$v $w 2>/dev/null | sed "s/^{/{\"set\": \"$v\", \"rep\": $rep, /" >> $O; done
  for pg in XregDash.qml XregPlots.qml XregPlotsShort.qml; do python3 create_timing.py $XREG_TEST_WORK/variants/$v $pg 2>/dev/null | sed "s/^{/{\"set\": \"$v\", \"rep\": $rep, /" >> $O; done
  python3 perf_timing.py $XREG_TEST_WORK/variants/$v 2>/dev/null | sed "s/^{/{\"set\": \"$v\", \"rep\": $rep, /" >> $O
done
done
print -r -- $O
