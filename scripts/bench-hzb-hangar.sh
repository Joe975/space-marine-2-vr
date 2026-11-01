#!/usr/bin/env bash
# HZB A/B in the hangar (docs/HZB_STEREO_2026-11-01.md): one launch per variant, holding one head pose.
# Variants alternate so a slow drift across the session can't pass for an HZB effect.
# Each launch: drive the menus into the save, wait for stereo pairs, settle, run bench_effects.py, close.
# Unattended, but the game window must keep the foreground: don't touch the keyboard or mouse meanwhile.
#
#   bash scripts/bench-hzb-hangar.sh default native default native
#   STATION=180:5 MODES=baseline,mono,baseline bash scripts/bench-hzb-hangar.sh default native
#   VISUAL=1 TAG=v bash scripts/bench-hzb-hangar.sh default native     # no profiler: eye snapshots to compare
#   KEYS=,keys:F8 VISUAL=1 TAG=m MODES=hzb_off,hzb_shared,hzb_off bash scripts/bench-hzb-hangar.sh live   # F8 once: diorama, twice: shoulder
#   TAG=f MODES=mono,mono,baseline bash scripts/bench-hzb-hangar.sh default    # flat reference
#   MODES=hzb_off,hzb_left,hzb_off bash scripts/bench-hzb-hangar.sh live   # one launch, hzb.txt switched live
# Eye snapshots are refused while the profiler runs, so a launch is either timed or VISUAL.
# Going to mono can cancel a scripted hold (clock_gap); a second mono step then measures it.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
WIN=$(cd "$REPO" && pwd -W)
PY=$REPO/.venv/Scripts/python.exe
[ -x "$PY" ] || PY=/c/Repos/sm2-vr/.venv/Scripts/python.exe  # worktrees share the main venv
OUT=$REPO/build/hzb_bench
LOG="$LOCALAPPDATA/sm2vr/sm2vr.log"
STATION=${STATION:-180:5}
STATIONS=${STATIONS:-$STATION}  # space-separated yaw:pitch list, one sweep each in the same launch
VISUAL=${VISUAL:-0}
[ "$VISUAL" = 1 ] || MODES=${MODES:-baseline,mono,baseline}
HOLD_S=${HOLD_S:-60}
SETTLE_S=${SETTLE_S:-15}
TAG=${TAG:-}
perf=" -PerfCampaign"; sweep=()
if [ "$VISUAL" = 1 ]; then perf=""; MODES=${MODES:-baseline}; HOLD_S=25; SETTLE_S=12; sweep=(--snapshots); fi
# The driver closes the game when this runs out, so it must outlast the sweep.
steps=$(echo "$MODES" | tr "," " " | wc -w)
count=$(echo "$STATIONS" | wc -w)
WAIT_S=${WAIT_S:-$((count * steps * (HOLD_S + 16) + 90))}
mkdir -p "$OUT"
n=0
for variant in "$@"; do
  n=$((n + 1))
  case "$variant" in
    native) extra=" -NativeHzb" ;;
    live) extra=" -HzbControl off" ;;
    default) extra="" ;;  # what a plain launch gets
    *) echo "unknown variant $variant (default, native or live)"; exit 2 ;;
  esac
  tag=hzb_${TAG}${n}_$variant
  dump=$WIN/build/ordinary/$(date +%Y-%m-%d)/$tag
  drive_log=$OUT/drive_$tag.log
  if [ "$(tasklist //fo csv | grep -ci 'space mar')" != "0" ]; then echo "$(date +%T) game already running; stop"; exit 1; fi
  echo "$(date +%T) launch $tag"
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WIN/scripts/drive-game.ps1" \
    -Launch "-Controls -TestDriver -ScriptedHead -EyeSnapshots$perf -Seconds 1500 -Tag $tag$extra" \
    -Do "continue${KEYS:-},wait:$WAIT_S" -RequireStereo -Close -OutDir "$WIN/build/hzb_bench/$tag" \
    > "$drive_log" 2>&1 &
  driver=$!
  ok=0
  for i in $(seq 1 330); do
    sleep 2
    kill -0 "$driver" 2>/dev/null || break
    if [ -f "$dump/shading.txt" ] && grep -qE "OrdinaryHeadsetRetention: .*pairs=[1-9]" "$LOG" 2>/dev/null; then ok=1; break; fi
  done
  if [ $ok = 1 ]; then
    echo "$(date +%T) $tag: pairs flowing; settling 30 s"
    sleep 30
    s=0
    for station in $STATIONS; do
      s=$((s + 1))
      label=$tag; [ "$count" = 1 ] || label=${tag}_s$s
      "$PY" -I "$REPO/scripts/bench_effects.py" "$dump" --label "$label" --modes "$MODES" \
        --station="$station" --hold-s "$HOLD_S" --settle-s "$SETTLE_S" "${sweep[@]}" > "$OUT/sweep_$label.log" 2>&1
      rc=$?
      echo "$(date +%T) $label at $station: sweep exit $rc"; [ "$rc" = 0 ] || ok=0
      grep -v "^analysis" "$OUT/sweep_$label.log"
    done
  else
    echo "$(date +%T) $tag: no stereo pairs; see $drive_log"
  fi
  # Same as the launcher's own shutdown; the launcher then removes the mod files.
  powershell.exe -NoProfile -Command "Get-Process | Where-Object { \$_.ProcessName -like 'Warhammer 40000 Space Marine 2*' } | Stop-Process -Force" >/dev/null 2>&1
  wait "$driver"
  tail -3 "$drive_log"
  [ $ok = 1 ] || exit 1
done
echo "$(date +%T) hzb bench done"
