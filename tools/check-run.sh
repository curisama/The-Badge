#!/usr/bin/env bash
# Judges one run log by the standard a person would read it with.
#
# 🚨 Build output and the run log end up in the same file. Grepping for 'panic'
#    once counted compiled file names (panic_handler.c) too and read as "died 6
#    times". So only run lines are looked at here (ones starting with a log
#    level, or carrying a boot marker).
set -u
L=${1:?usage: check-run.sh <logfile>}
RUN=$(mktemp)
sed 's/\x1b\[[0-9;]*m//g' "$L" | grep -E '^[IWED] \(|^rst:0x|Guru Meditation|Backtrace' > "$RUN"

draw=$(grep -cE 'priv TX buffer|Draw bitmap failed|transmit \(queue\) color' "$RUN")
dead=$(grep -cE 'task_wdt: Task watchdog|Guru Meditation|abort\(\) was called' "$RUN")
boot=$(grep -cE '^rst:0x' "$RUN"); boot=$((boot > 0 ? boot - 1 : 0))
# 🚨 Harmless errors with a known cause are counted separately — not erased.
#    i2s_channel_disable "not been enabled yet":
#      The microphone codec handle covers both directions, so closing it tries
#      to shut down the unused transmit channel too. The recording itself is
#      logged as fine (all 15 survived in the 09-08 check).
#      It is inside a vendor component and is left alone. The firmware's own
#      health check uses the same standard.
KNOWN='has not been enabled yet'
known=$(grep -E '^E \(' "$RUN" | grep -cE "$KNOWN")
err=$(grep -E '^E \(' "$RUN" | grep -vcE "$KNOWN")

echo "draw failures ${draw}   ← has to be 0 to pass"
echo "deaths        ${dead}"
echo "unexpected reboots ${boot}"
echo "E errors      ${err}   (${known} explained as harmless, counted apart)"
if [ "$err" -gt 0 ]; then echo "── unexplained E errors"; grep -E '^E \(' "$RUN" | grep -vE "$KNOWN" | sort -u | head; fi
grep -E 'stress test end' "$RUN" | sed 's/^.*badge: //'
rm -f "$RUN"
[ "$draw" -eq 0 ] && [ "$dead" -eq 0 ] && [ "$boot" -eq 0 ] && [ "$err" -eq 0 ] \
  && echo "→ pass" || { echo "→ not a pass"; exit 1; }
