#!/bin/zsh
# Every offline suite of the XREG-010 GX app against this repo's code, then one summary line per suite. Logs and every
# temporary file go to a new folder under $TMPDIR, printed first; nothing is written into the repo.
#   run_all.sh [bridge] [package] [review] [qml]     (no argument: all four)
# bridge   bridge/: the bridge (test_bridge, test_fastlane, test_flow_merge, test_health_windows, test_switch_hold), the updater
#          (test_update) and install.sh on the fake GX (test_install_edges, test_real_update_data). Runs alone, before
#          the other groups, as these suites time things by the wall clock.
# package  build_package.py into the log folder, unsigned; it runs tools/sim_install.py on the package.
# review   review/: the 2026-10-30 install.sh and updater review harnesses, on a copy of gx_app packaged as
#          0.3+1761800000. upd_test and crash_test print OK/BAD per case; the others print what happened, no verdict.
# qml      qml/: the plugin's QML on PySide6 against stub Victron.VenusOS modules, numbers and text only, no images.
#          sweep_test, long_test and health_test (speed/) compare the plugin with the first speed set's pages kept in
#          speed/ref_speed1/, on data made at each run: longcases.py from the bridge's own Long Term tables (1 MB, so
#          not kept here), healthdata.py from a fixed seed.
# Needs:
#   python3.12 for bridge/ (the run line the suites name; the bridge's Python on Venus OS); nothing in them is 3.12-only
#   python3 with PySide6 6.x for qml/ (and for package and review, which need no PySide6); async_check.py needs Metal
#   node (test_bridge's long-term parser test), openssl (test_update, upd_test), /bin/dash (the fake GX's shell,
#   /bin/sh without it), unzip (test_real_update_data runs Victron's update-data.sh with it)
#   the cache that fetch_deps.sh fills (gui-v2 1.3.22, Victron's update-data.sh); it runs first, network on first use
#   ~/Projects/Cloud Workspace/vercel/leaderboards.js for one test_bridge test (XREG_CLOUD_WORKSPACE moves it)
# XREG_PLUGIN runs qml/ on another plugin folder; XREG_BRIDGE moves test_bridge's and test_update's bridge.
setopt nullglob
T=${0:A:h}
GX=${T:h}
PLUGIN=${XREG_PLUGIN:-$GX/plugin}
SCHEMA=$GX/xregulator/bridge/settings_schema.json
L=$(mktemp -d -t xreg-gx-tests)
print -r -- "logs: $L"
export PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 QT_QPA_PLATFORM=offscreen
export XREG_TEST_WORK=$L/work XREG_REVIEW_WORK=$L/review_work
mkdir -p $XREG_TEST_WORK
PYB=python3.12
(( $+commands[python3.12] )) || PYB=python3
$T/fetch_deps.sh > $L/fetch_deps.log 2>&1 || { cat $L/fetch_deps.log; exit 1 }
want=($@)
(( $#want )) || want=(bridge package review qml)

ut() {   # unittest log: "Ran N tests; OK" / "Ran N tests; FAILED (failures=1)"
  print -r -- "$(grep -E '^Ran [0-9]+ test' $1 | tail -1 | cut -d' ' -f1-3); $(grep -E '^(OK|FAILED)' $1 | tail -1)"
}
last() {   # last line of a log matching a pattern
  grep -E "$2" $1 | tail -1
}
okbad() {   # review harnesses that print OK/BAD per case
  print -r -- "$(grep -c '^OK ' $1) OK, $(grep -c '^BAD ' $1) BAD"
}
note() {   # note <group> <name> <exit> <summary>
  print -r -- "$2: $4 (exit $3)" >> $L/summary_$1.txt
}

bridge() {
  cd $T/bridge
  local t
  for t in test_bridge test_fastlane test_flow_merge test_health_windows test_switch_hold test_update test_real_update_data test_install_edges; do
    $PYB -B $t.py > $L/$t.log 2>&1
    note bridge $t $? "$(ut $L/$t.log)"
  done
}

package() {
  python3 -B $GX/build_package.py $L/package > $L/build_package.log 2>&1
  note package build_package+sim_install $? "$(last $L/build_package.log '^SUMMARY'); $(last $L/build_package.log '^venus-data.zip' | cut -c1-80)"
}

review() {
  cd $T/review
  python3 -B -c 'import reviewenv; reviewenv.work()' > $L/review_prep.log 2>&1 || { note review prep $? "package of the gx_app copy failed, see review_prep.log"; return }
  local t
  for t in new delayed new3 upd clock rc recover crash; do
    timeout 2400 python3 -B ${t}_test.py > $L/review_$t.log 2>&1
    local rc=$?
    case $t in
      upd|crash) note review ${t}_test $rc "$(okbad $L/review_$t.log)" ;;
      *) note review ${t}_test $rc "printed $(wc -l < $L/review_$t.log | tr -d ' ') lines, no verdict" ;;
    esac
  done
}

qml() {
  local Q=$T/qml sz sc
  local -a lp
  cd $Q/plugin
  for sz in SevenInch FiveInch Portrait; do
    python3 layout.py $PLUGIN $SCHEMA $sz empty=$Q/fixtures/empty.json demo=$Q/fixtures/demo.json > $L/layout_$sz.log 2>&1 &
    lp+=($!)
  done
  for sz in SevenInch FiveInch Portrait; do
    wait $lp[1]
    note qml layout_$sz $? "$(last $L/layout_$sz.log SUMMARY)"
    shift lp
  done
  for sz sc in SevenInch Dark FiveInch Dark Portrait Dark SevenInch Light; do
    python3 run.py $PLUGIN $SCHEMA $sz $sc > $L/run_${sz}_$sc.log 2>&1
    note qml run_${sz}_$sc $? "$(last $L/run_${sz}_$sc.log SUMMARY)"
  done
  python3 gesture.py $PLUGIN $SCHEMA $Q/fixtures/render_data.json > $L/gesture.log 2>&1
  note qml gesture $? "$(last $L/gesture.log SUMMARY)"
  timeout 120 python3 compile_all.py $PLUGIN > $L/compile.log 2>&1
  note qml compile_all $? "$(last $L/compile.log 'DONE compile')"
  cd $Q/switch
  python3 toggle_test.py $PLUGIN $SCHEMA > $L/toggle.log 2>&1
  note qml toggle $? "$(last $L/toggle.log SUMMARY)"
  python3 health_test.py $PLUGIN > $L/health.log 2>&1
  note qml health_windows $? "$(last $L/health.log SUMMARY)"
  cd $Q/tile
  for sc in Dark Light; do
    python3 flow_run.py $PLUGIN scenarios.json $sc > $L/tile_$sc.log 2>&1
    note qml tile_$sc $? "$(last $L/tile_$sc.log SUMMARY)"
  done
  cd $Q/devices
  for sz in SevenInch FiveInch; do for sc in Dark Light; do
    python3 devices_run.py $PLUGIN $sz $sc > $L/devices_${sz}_$sc.log 2>&1
    note qml devices_${sz}_$sc $? "$(last $L/devices_${sz}_$sc.log SUMMARY)"
  done; done
  cd $Q/gxapp
  for sz in SevenInch FiveInch Portrait; do for sc in Dark Light; do
    python3 gxapp_test.py $PLUGIN $SCHEMA $sz $sc > $L/gxapp_${sz}_$sc.log 2>&1
    note qml gxapp_${sz}_$sc $? "$(last $L/gxapp_${sz}_$sc.log SUMMARY)"
  done; done
  cd $Q/speed
  for sc in Dark Light; do
    python3 new_test.py $PLUGIN $sc > $L/new_$sc.log 2>&1
    note qml new_test_$sc $? "$(last $L/new_$sc.log SUMMARY)"
  done
  python3 sweep_test.py $Q/speed/ref_speed1 $PLUGIN > $L/sweep.log 2>&1
  note qml sweep_test $? "$(last $L/sweep.log SUMMARY)"
  python3 longcases.py $XREG_TEST_WORK/longcases.json > $L/longcases.log 2>&1
  python3 long_test.py $Q/speed/ref_speed1 $PLUGIN $XREG_TEST_WORK/longcases.json > $L/long.log 2>&1
  note qml long_test $? "$(last $L/long.log SUMMARY)"
  python3 healthdata.py $XREG_TEST_WORK/health.json > $L/healthdata.log 2>&1
  python3 health_test.py $Q/speed/ref_speed1 $PLUGIN $XREG_TEST_WORK/health.json > $L/health_session.log 2>&1
  note qml health_session $? "$(last $L/health_session.log SUMMARY)"
  # The synchronous twin of the plugin: every Shape's "asynchronous: true" line taken out
  local sync=$XREG_TEST_WORK/plugin_sync f
  mkdir -p $sync
  for f in $PLUGIN/*.qml; do sed -E '/^[[:space:]]*asynchronous: true([[:space:]]|$)/d' $f > $sync/${f:t}; done
  cd $Q/timing
  python3 async_check.py $PLUGIN $sync > $L/async_check.log 2>&1
  note qml async_check $? "$(last $L/async_check.log 'SUMMARY')"
}

(( ${want[(Ie)bridge]} )) && bridge
pids=()
for g in package review qml; do
  (( ${want[(Ie)$g]} )) && { $g & pids+=($!) }
done
(( $#pids )) && wait $pids
for g in bridge package review qml; do
  [[ -f $L/summary_$g.txt ]] && { print -r -- "== $g"; cat $L/summary_$g.txt }
done
print -r -- "logs: $L"
