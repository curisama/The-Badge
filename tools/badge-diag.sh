#!/bin/bash
# Pulls out in one go why the badge rebooted.
#
#   ./tools/badge-diag.sh          read the record (erases nothing)
#   ./tools/badge-diag.sh --clear  empty the record (only when a person asks for it)
#
# 🚨 The record never erases itself. Reading it means plugging in, and erasing
#    on plug-in would make it useless (that is how the battery journal was lost
#    on 09-06).
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-/dev/ttyACM0}

# 🚨 Windows' COMx is not a file. [ -e COM6 ] is always false even with the badge
#    plugged in, so this used to stop here saying "no board found" (09-09).
#    For a COMx the existence check is skipped and Python decides by opening it.
port_missing() {
    case "$1" in
        COM[0-9]*|com[0-9]*) return 1 ;;
        *) [ -e "$1" ] && return 1 || return 0 ;;
    esac
}

if port_missing "$PORT"; then
    echo "✗ no board found ($PORT)"
    echo "  Windows:  PORT=COM6 ./tools/badge-diag.sh   (check the number in Device Manager)"
    echo "  WSL   :  usbipd attach --wsl --busid 1-1"
    exit 1
fi

echo "════════ 1. boot record (reset reason · uptime · what it was doing) ════════"
timeout 25 python3 - "$PORT" <<'PYEOF'
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 15
# Filtered by tag, so changing the wording does not leak.
TAGS = (') rst:', ') badge:', ') batt:', 'rst:0x', 'Light sleep')
while time.time() < end:
    t = s.readline().decode('utf-8', 'replace').rstrip()
    if t and any(k in t for k in TAGS):
        print(t)
PYEOF

echo
echo "════════ 2. core dump (a panic names where it died) ════════"
if [ ! -f build/badge_fw.elf ]; then
    echo "  (no build/badge_fw.elf — run idf.py build first)"
elif ! command -v idf.py >/dev/null 2>&1; then
    echo "  (not an IDF environment —  . ~/esp/esp-idf/export.sh  first)"
else
    OUT=$(idf.py -p "$PORT" coredump-info 2>&1)
    # An empty partition is full of 0xff, which comes out as "version 0xffff is not supported"
    if echo "$OUT" | grep -qiE 'No core dump|not found|is empty|0xffff\" is not supported|Incorrect size of core dump'; then
        echo "  no dump = it has never died in a panic"
        echo "  → if the boot record says brownout[9]/power glitch[14], it is a power problem;"
        echo "    watchdog[5~7] means something never let the CPU go"
    else
        HIT=$(echo "$OUT" | grep -A40 -iE 'crashed task|PC:|backtrace|panic|Exception' | head -50)
        if [ -n "$HIT" ]; then echo "$HIT"; else
            echo "  the dump was read but nothing familiar is in it. Full output:"
            echo "$OUT" | tail -25
        fi
    fi
fi

if [ "${1:-}" = "--clear" ]; then
    echo
    echo "════════ emptying the record ════════"
    idf.py -p "$PORT" coredump-erase 2>&1 | tail -2
    echo "  ※ the boot record (NVS) rolls over by itself after twelve, so it is left alone"
fi

echo
echo "════════ how to read this ════════"
cat <<'HELP'
  panics/watchdogs repeating at 'screen on'  → the wake path is the culprit
  brownout[9] · power glitch[14]             → a voltage problem (check the battery level too)
  the previous uptime is always short        → it is dying right after boot
  there is a core dump                       → that is the answer. It names the file and line
HELP
