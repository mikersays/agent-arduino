#!/bin/bash
# UNO Q Linux-side LED demo: cycle the user RGB LED (LED1) through the 7
# on-colors, flash a heartbeat, then clean up. Needs no sudo.
#
# Run with:  bash led-cycle.sh
set -euo pipefail

LEDS=/sys/class/leds
R="$LEDS/unoq:user-red1"
G="$LEDS/unoq:user-green1"
B="$LEDS/unoq:user-blue1"

set_rgb() { # set_rgb <r> <g> <b>  (each 0 or 1)
  echo "$1" > "$R/brightness"
  echo "$2" > "$G/brightness"
  echo "$3" > "$B/brightness"
}

cleanup() {
  for l in "$R" "$G" "$B"; do
    echo none > "$l/trigger"
    echo 0 > "$l/brightness"
  done
}
trap cleanup EXIT

echo "Color wheel..."
for rgb in "1 0 0" "1 1 0" "0 1 0" "0 1 1" "0 0 1" "1 0 1" "1 1 1"; do
  set_rgb $rgb
  sleep 0.4
done
set_rgb 0 0 0

echo "Heartbeat on green for 4 s..."
echo heartbeat > "$G/trigger"
sleep 4

echo "Done (cleanup restores everything off)."
