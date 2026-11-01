#!/usr/bin/env bash
# Effects benchmark campaign (docs/validation/effects-benchmark-2026-10-23.md): one hangar launch per
# effects pack. Each launch: start the headset launcher with auto-continue into the current save,
# wait for stereo pairs, settle 30 s, run bench_effects.py, stop the game, wait for the launcher's
# cleanup (DLL and test pack removed). Unattended, but the game window must keep the foreground for
# auto-continue: do not use the desktop meanwhile.
#
#   bash scripts/bench-effects-campaign.sh default bloom lightshafts sun_flare camera_fx all
#   VISUAL=1 bash scripts/bench-effects-campaign.sh default all   # no profiler: short holds + eye snapshots
#
# pack = default (the controls pack) or a folder of build/effects_bench (scripts/build_effects_bench.py).
# MODES overrides the sweep (default baseline,flares,all,everything,baseline).
set -u
REPO=/c/Repos/sm2-vr
OUT=$REPO/build/effects_bench
LOG="$LOCALAPPDATA/sm2vr/sm2vr.log"
MODES=${MODES:-baseline,flares,all,everything,baseline}
VISUAL=${VISUAL:-0}
mkdir -p "$OUT"
for pack in "$@"; do
  if [ "$VISUAL" = 1 ]; then tag=bench_fxvis_$pack; perf=(); sweep=(--hold-s 25 --settle-s 12 --snapshots)
  else tag=bench_fx2_$pack; perf=(-PerfCampaign); sweep=(); fi
  launch_log=$OUT/launch_$tag.log
  extra=()
  [ "$pack" != default ] && extra=(-EffectsPack "C:/Repos/sm2-vr/build/effects_bench/$pack/SM2VR_FirstPerson_Test.pak")
  if [ "$(tasklist //fo csv | grep -ci 'space mar')" != "0" ]; then echo "$(date +%T) game already running; stop"; exit 1; fi
  echo "$(date +%T) launch $pack"
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:/Repos/sm2-vr/scripts/run-ordinary-headset.ps1" \
    -Controls -TestDriver -ScriptedHead -EyeSnapshots -FlareHideControl -AutoContinue -Seconds 1500 -Tag "$tag" "${perf[@]}" "${extra[@]}" \
    > "$launch_log" 2>&1 &
  ok=0
  for i in $(seq 1 360); do
    sleep 2
    if grep -q "auto-continue done" "$launch_log" 2>/dev/null && grep -qE "OrdinaryHeadsetRetention: .*pairs=[1-9]" "$LOG" 2>/dev/null; then ok=1; break; fi
    if grep -qE "removed mod dll|game exited" "$launch_log" 2>/dev/null; then break; fi
  done
  if [ $ok = 1 ]; then
    echo "$(date +%T) $pack: pairs flowing; settling 30 s"
    sleep 30
    dump=C:/Repos/sm2-vr/build/ordinary/$(date +%Y-%m-%d)/$tag
    "$REPO/.venv/Scripts/python.exe" -I "$REPO/scripts/bench_effects.py" "$dump" --label "$pack" --modes "$MODES" "${sweep[@]}" > "$OUT/sweep_$tag.log" 2>&1
    echo "$(date +%T) $pack: sweep exit $?"; grep -v "^analysis" "$OUT/sweep_$tag.log"
  else
    echo "$(date +%T) $pack: no stereo pairs (focus lost or load stalled); see $launch_log"
  fi
  # Same as the launcher's own shutdown (Stop-Sm2OwnedGame kills after a 20 s close request).
  powershell.exe -NoProfile -Command "Get-Process | Where-Object { \$_.ProcessName -like 'Warhammer 40000 Space Marine 2*' } | Stop-Process -Force" >/dev/null 2>&1
  for i in $(seq 1 120); do sleep 2; grep -qE "removed mod dll|MOD DLL STILL|retained" "$launch_log" && break; done
  wait
  tail -3 "$launch_log"
  [ $ok = 1 ] || exit 1
done
echo "$(date +%T) campaign done"
