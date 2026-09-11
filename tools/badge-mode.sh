#!/bin/bash
# Says what state the badge is in right now.
# More useful than "did it enter download mode" is "can it be flashed right now" — that is what this looks at.
PORT=${1:-/dev/ttyACM0}

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
    echo "✗ no board found (no $PORT)"
    echo "  → check the USB cable."
    echo "     on Windows:  ./tools/badge-mode.sh COM6"
    echo "     on WSL:      usbipd attach --wsl --busid 1-1"
    exit 1
fi

# Reads the line the ROM prints at boot. boot:0x0 = download, boot:0xb = a normal boot.
BANNER=$(timeout 8 python3 - "$PORT" <<'PY' 2>/dev/null
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 4
out = []
while time.time() < end:
    l = s.readline().decode('utf-8', 'replace').strip()
    if l: out.append(l)
print("\n".join(out))
PY
)

echo "$BANNER" | grep -q 'waiting for download' && {
    echo "✓ download mode (the ROM is waiting for commands)"
    echo "$BANNER" | grep -i 'boot:' | head -1
    exit 0
}
echo "$BANNER" | grep -q 'SPI_FAST_FLASH_BOOT' && {
    echo "✓ normal boot — the app is running"
    echo "$BANNER" | grep -i 'boot:' | head -1
}

# Either way, what is really wanted is this: can it be flashed right now?
echo "── checking whether it can be flashed"
if esptool.py --chip esp32s3 -p "$PORT" --before default_reset --after hard_reset chip_id 2>&1 | grep -q 'Chip is ESP32-S3'; then
    echo "✓ it can be flashed. No need to hold BOOT"
else
    echo "✗ no answer → hold BOOT, unplug and replug the cable, and run this again"
fi
