#!/usr/bin/env bash
# Makes this repository runnable on a new computer.
#
#   ./tools/setup.sh --sim-only   simulator only (no badge, no ESP-IDF needed)
#   ./tools/setup.sh              firmware too (ESP-IDF has to be installed already)
set -eu
cd "$(dirname "$0")/.."
SIM_ONLY=0
[ "${1:-}" = "--sim-only" ] && SIM_ONLY=1

echo "════ 1. the secrets file"
if [ -f main/secrets.h ]; then
    echo "  already there"
else
    cp main/secrets.example.h main/secrets.h
    echo "  created main/secrets.h — open it and fill in the SSID and password"
fi

echo "════ 2. other people's code (LVGL and so on)"
if [ -d managed_components/lvgl__lvgl ] || [ -d sim/lvgl ]; then
    echo "  already there"
elif [ "$SIM_ONLY" = 1 ]; then
    # 🚨 The simulator needs nothing but the LVGL sources. It runs this far
    #    without downloading several GB of ESP-IDF — for places like a company
    #    PC where a heavy install is awkward.
    #
    # 🚨 It used to clone by hand into managed_components/. Building firmware on
    #    the same machine later was then refused by idf.py ("no component_hash
    #    or CHECKSUMS.json"). So it goes into sim/lvgl instead — build.py looks
    #    in both places (caught on the company PC, 09-09).
    V=$(grep -A2 'lvgl/lvgl' main/idf_component.yml | grep version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    echo "  fetching LVGL ${V} (simulator only, sim/lvgl)"
    git clone --depth 1 -b "v${V}" https://github.com/lvgl/lvgl.git sim/lvgl
else
    command -v idf.py >/dev/null || { echo "  ✗ not an ESP-IDF environment. Run . ~/esp/esp-idf/export.sh first"; exit 1; }
    idf.py reconfigure >/dev/null
    echo "  fetched"
fi

echo "════ 3. building the simulator"
bash sim/build.sh

if [ "$SIM_ONLY" = 0 ]; then
    echo "════ 4. building the firmware"
    command -v idf.py >/dev/null && idf.py build || echo "  skipped (not an ESP-IDF environment)"
fi

echo
echo "→ done."
echo "   simulator:  python3 sim/server.py   then open localhost:8791 in a browser"
# 🚨 The name the badge appears under differs by machine. Linux is /dev/ttyACM0,
#    macOS is /dev/cu.usbmodem* (the ESP32-S3 has native USB CDC, so it comes up
#    as usbmodem), Windows is COMx. Documenting only the Linux one leaves people
#    lost everywhere else.
# 🚨 `[ condition ] && echo` must not be the last line. A false condition exits
# the script with 1, and a caller running set -e stops there — the setup script
# died without printing its own closing notes (09-09).
if [ "$SIM_ONLY" = 0 ]; then
    case "$(uname -s)" in
        Darwin) echo "   flash:  idf.py -p \$(ls /dev/cu.usbmodem* | head -1) flash monitor" ;;
        *)      echo "   flash:  idf.py -p /dev/ttyACM0 flash monitor" ;;
    esac
fi
exit 0
