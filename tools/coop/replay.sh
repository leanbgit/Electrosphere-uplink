#!/bin/bash
# replay.sh NAME REC FIELDS [bench options...]
#   Replays an input recording (made with record_mission.bat) headless, in
#   English, into $WORK/runs/NAME.*: the recording and its card snapshots are
#   copied first, so parallel runs never share memory-card files. Always writes
#   a per-field hash log (runs/NAME.tsv) and the console log (runs/NAME.log).
# Environment:
#   GAME  folder holding ACE3.BIN, disc/ and translation/   (default ./game)
#   BIN   the bench binary            (default build-headless/ac3_headless)
#   WORK  output folder                (default .)
set -e
N=$1; R=$2; F=$3; shift 3
GAME=${GAME:-game}; WORK=${WORK:-.}
BIN=${BIN:-$(dirname "$0")/../../build-headless/ac3_headless}
mkdir -p "$WORK/runs"
cp "$R" "$WORK/runs/$N.ac3i"
for s in 1 2; do [ -f "$R.card$s.mcd" ] && cp "$R.card$s.mcd" "$WORK/runs/$N.ac3i.card$s.mcd"; done
# the game looks for game/translation relative to the working directory
cd "$WORK"; [ -e game ] || ln -s "$GAME" game
exec "$BIN" --english --boot --window --exe game/ACE3.BIN \
  --disc "game/disc/Ace Combat 3 - Electrosphere (Japan, Asia) (Disc 1) (Rev 1).bin" \
  --play-input "runs/$N.ac3i" --frames "$F" --coop-hash "runs/$N.tsv" "$@" > "runs/$N.log" 2>&1
