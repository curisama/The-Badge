# Carrying on from another computer

Written down so that home and work can both be worked on in the same state.
Cloning the repository is meant to be enough, but **four things are kept out of
git**. Those are all you have to bring.

## What is not in git (deliberately)

| | why | how |
|---|---|---|
| `main/secrets.h` | it holds the WiFi password | make it by hand, in the form below |
| `managed_components/` | somebody else's code (LVGL and 20 others) | `idf.py reconfigure` fetches it |
| `sdkconfig` | it is regenerated on each machine | generated from `sdkconfig.defaults` |
| `build/` | output | build again |

🚨 Do not put `sdkconfig` in git. Every setting a person touched has to be in
   `sdkconfig.defaults` — anything missing there quietly produces different
   firmware on another machine (things like PSRAM speed, the partition table
   and light sleep).

## Using only the simulator (no badge)

Most of what was built was checked this way — the water particles, the orb rotation, fonts, game logic.

```bash
git clone https://github.com/curisama/The-Badge && cd The-Badge
./tools/setup.sh --sim-only        # fetch LVGL only and build the simulator
python3 sim/server.py              # then open localhost:8791 in a browser
```

## Building the firmware too

ESP-IDF v5.5 is needed (several GB to download).

```bash
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.5.5 ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh

cd The-Badge
cp main/secrets.example.h main/secrets.h   # fill in the password
idf.py build
idf.py -p /dev/ttyACM0 flash monitor       # COMx on Windows
```

## Rules for working from both machines

- **Branches**: everyday work is on `main`; experiments get their own branch.
- **`git pull` before starting.** Fixing something on one side and again on the other makes a tedious merge.
- **Push when finished.** Even half-done work is better pushed on a branch.

## The checks to run before and after touching anything

```bash
./tools/regress.sh              # have any of the 64 past faults come back (source only)
python3 tools/sim-exit-check.py # does any board die on the way home
python3 tools/sim-stress.py     # a shaken clock and rough handling
```

With a badge to hand, these too:

```bash
./tools/badge-diag.sh           # reset reasons and the battery journal (erases nothing)
./tools/check-run.sh <log>      # judges a run log by a person's standard
```
