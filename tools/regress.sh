#!/usr/bin/env bash
# Checks in the sources whether anything we have been bitten by has come back.
#
# 🚨 The same mistake has been made twice (freeing memory before the picture —
#    fixed in the orbs, repeated in water). Remembering where it was fixed is
#    not good enough. The 'trace' of the fix is pinned down by a check.
set -u
cd "$(dirname "$0")/.."
FAIL=0
ok()  { printf '  ✓ %s\n' "$1"; }
bad() { printf '  ✗ %s\n' "$1"; FAIL=1; }
has() { grep -qF "$2" "$1" 2>/dev/null; }

echo "════ drawing ════"
has main/display.c ".use_psram = false" \
  && ok "the draw buffer is in internal RAM (in PSRAM it grabs a scratch buffer per transfer and failed 13,943 times)" \
  || bad "the draw buffer is back in PSRAM"
# 🚨 It used to look for ".buffer_height = 16" literally. That value is tuned
#    against measured performance (raised to 24 on 09-09 with reasons given).
#    A check tied to a number called a perfectly good change wrong — a check
#    has to look at the meaning, not the name. The meaning to hold is "do not
#    hold the whole screen at once" (internal RAM blows up).
BH=$(grep -oE "\.buffer_height = [0-9]+" main/display.c | grep -oE "[0-9]+")
[ -n "$BH" ] && [ "$BH" -ge 8 ] && [ "$BH" -le 64 ] \
  && ok "only ${BH} rows go out at a time (the whole screen is not held at once)" \
  || bad "the band height looks wrong (currently '${BH:-none}')"
sed -n "/static esp_err_t panel_cmd/,/^}/p" main/display.c | grep -q port_lock \
  && ok "panel commands take the lock (changing brightness fought drawing over SPI)" \
  || bad "panel_cmd does not take the lock"

echo "════ radio ════"
has main/ble/hid_mouse.c "esp_hid_gap_deinit()" \
  && ok "bringing BLE down brings HID GAP down too (left up, it never comes back)" \
  || bad "esp_hid_gap_deinit is missing"

echo "════ sound ════"
has main/port_esp.c "tone_muted" \
  && ok "muted, it does not hold the codec (it used to push silence and burn power)" \
  || bad "it holds the codec while muted"
grep -q "s_tone_held ? 20000000" main/port_esp.c \
  && ok "even when held, it lets go after 20 quiet seconds" || bad "it holds the codec indefinitely"

echo "════ the journal ════"
has main/port_esp.c "JRN_BREAK" \
  && ok "unplugging after a charge does not wipe the journal (a boundary is marked and it carries on)" \
  || bad "the journal is wiped on unplug"
# The **behaviour** is checked, not the wording: does a rising level re-baseline?
grep -qE 'pct > s_last_pct' main/port_esp.c \
  && grep -qE 's_last_pct = pct;[^\n]*' main/port_esp.c \
  && ok "a level pushed down by load is not counted twice" || bad "double counting of the level is back"

echo "════ the way out (never free memory before the picture) ════"
grep -A6 "^void orb_stop" main/apps/orb.c | grep -q "lv_obj_delete(s_canvas)" \
  && ok "orbs: the picture is deleted first" || bad "orb_stop frees the memory first"
grep -A8 "^void water_stop" main/apps/app_water.c | grep -q "lv_obj_delete(s_field)" \
  && ok "water: the picture is deleted first" || bad "water_stop frees the memory first"
grep -A3 "^static void draw_cb" main/apps/app_water.c | grep -q "if (!s_x" \
  && ok "water: nothing is drawn after the free" || bad "water's draw_cb has no guard"

echo "════ axes and directions (wrong several times on the hardware) ════"
grep -q "s_mvx -= gy" main/apps/app_games.c && grep -q "s_mvy -= gx" main/apps/app_games.c \
  && ok "marble: the IMU axes being 90 degrees off the screen is accounted for" || bad "the marble axes are back to front"
grep -qE "yy = -\(float\)dy / (ORB_R|s_R)" main/apps/orb.c \
  && ok "orbs: up on screen is north" || bad "the orbs' north and south are flipped"
grep -q "float lat = gy;" main/apps/app_games.c \
  && ok "tilt board: it goes the way it is tilted" || bad "the tilt board's left and right are reversed"

echo "════ air mouse ════"
LAST=$(grep -n 'air_paint()' main/apps/app_mouse.c | tail -1 | cut -d: -f1)
BTN=$(grep -n 's_air_l = mk_click_btn' main/apps/app_mouse.c | cut -d: -f1)
[ -n "$LAST" ] && [ -n "$BTN" ] && [ "$LAST" -gt "$BTN" ] \
  && ok "it shows and hides after the buttons exist (called first, they stay hidden)" \
  || bad "air_paint is called before the buttons — the left and right buttons never appear"

echo "════ turning pages ════"
grep -q 'build_home();' main/launcher.c && grep -A6 's_swiped = true;' main/launcher.c | grep -q 'build_home' \
  && ok "changing page rebuilds home (merely showing it again leaves the screen as it was)" \
  || bad "the page changes but home is not rebuilt"

grep -qE 's_prev_p = -2;' main/launcher.c \
  && ok "a rebuilt home always writes the battery number once (without it, LVGL's default 'Text' stays)" \
  || bad "the battery label on a new home can be left as the default text"
grep -B2 's_batt_timer = lv_timer_create' main/launcher.c | grep -q 'lv_timer_delete' \
  && ok "rebuilding home deletes the old battery timer (otherwise they pile up with every page turn)" \
  || bad "timers pile up when home is rebuilt"

echo "════ does it run anywhere ════"
# 🚨 -Wl,--start-group is GNU ld's. Apple's linker does not know it and the link dies
grep -q 'sys.platform == "darwin"' sim/build.py \
  && ok "the Mac linker is handled separately" || bad "the link dies on a Mac (--start-group)"
# 🚨 The name the badge appears under differs by machine — documenting only Linux leaves people lost elsewhere
grep -q "cu.usbmodem" tools/setup.sh \
  && ok "the port note matches the machine" || bad "the port note covers only Linux"
# 🚨 An absolute path written into the tool stops it running at all on another
#    machine. Do not check for one particular home directory — that only finds
#    the one machine it was written on, and puts somebody's username in a public
#    repository. Any absolute path outside the repo is the fault.
python3 - <<'EOF' && ok "no machine-specific path is written into the asset tool" || bad "an absolute path is written into mkassets.py"
import re, sys
src = open("tools/mkassets.py", encoding="utf-8").read()
bad = re.findall(r"""["'](/home/[^"']*|/Users/[^"']*|/mnt/[a-z]/[^"']*|[A-Za-z]:\\[^"']*)["']""", src)
if bad:
    print("  absolute path in the source:", " · ".join(sorted(set(bad)))); sys.exit(1)
EOF
# 🚨 Putting the simulator's LVGL in managed_components makes idf.py refuse it later
! grep -q "managed_components/lvgl__lvgl$" tools/setup.sh \
  && ok "the simulator's LVGL lives in sim/lvgl" || bad "the simulator's LVGL is dirtying managed_components"
# 🚨 It has to stand up on Windows too — Python drives the build, win_compat.h is the POSIX shim
[ -f sim/build.py ] && [ -f sim/win_compat.h ] \
  && ok "the simulator stands up on Windows too" || bad "Windows support is missing"
# 🚨 Multi-line logs are joined into one line first, so there are no false positives

echo "════ the handover documents ════"
# 🚨 These are the only authority the next person and the next LLM read. Without them nobody can follow.
[ -f README.md ] && ok "there is a README" || bad "README.md is missing"
[ -f NOTICE ]    && ok "there is a third-party notice" || bad "NOTICE is missing"
[ -f LICENSE ]   && ok "there is a licence" || bad "LICENSE is missing"
# Says so if the code has run well ahead of the documents (a nudge to fix it, not a failure)
if [ -f docs/HANDOFF.md ]; then
  NEW=$(git log --oneline -20 --format=%H -- main/ tools/ 2>/dev/null | head -1)
  DOC=$(git log --oneline -1 --format=%H -- docs/HANDOFF.md 2>/dev/null)
  AHEAD=$(git rev-list --count "$DOC..$NEW" -- main/ tools/ 2>/dev/null || echo 0)
  [ "${AHEAD:-0}" -gt 3 ] \
    && echo "  ⚠ the code is ${AHEAD} commits ahead of the handover documents — check whether something needs fixing"
fi

echo "════ is the bench off ════"
# 🚨 Flashed with it on, the badge opens apps by itself on every boot and takes the controls away
grep -qE "^add_compile_definitions\(BADGE_APPBENCH" CMakeLists.txt \
  && bad "the app bench is on" || ok "the app bench is off"

echo "════ are the check hooks at file scope ════"
# 🚨 A function nested inside another builds fine under GCC's nested-function
#    extension but cannot be called from outside. games_debug_play_bricks was
#    hidden that way once.
awk '/^static void brk_touch/,/^}/' main/apps/app_games.c | grep -q "games_debug" \
  && bad "a check function is nested inside brk_touch" || ok "the check functions are at file scope"

echo "════ waking the IMU ════"
# 🚨 The first readings after a sleep cannot be trusted — used as "level", the board sticks to one edge
grep -q "imu_settled" main/port_esp.c \
  && ok "right after waking it answers that it does not know yet" || bad "it hands over what it read the instant it woke"
# 🚨 On 09-09 water dropped "pin the attitude level on open" and moved to real
#    gravity, so there is no baselining left at all — the old check was looking
#    for code that no longer exists. The meaning to hold now is "do not use the
#    IMU's values while they cannot be trusted".
grep -q "bool have = port_imu_accel3" main/apps/app_water.c \
  && grep -q "if (!have)" main/apps/app_water.c \
  && ok "water substitutes a value while the IMU cannot be trusted" || bad "water uses values it cannot trust"

echo "════ starting BLE ════"
# 🚨 esp_bt_controller does not return an error when it cannot allocate — it
#    asserts inside itself (BLE assert emi.c 164) and the interrupt watchdog
#    reboots the board. Every error path in port_hid_start() is unreachable in
#    the one case that matters. Measured on hardware: BLE needs about 62 KB of
#    internal RAM, and the boot-time clock sync leaves 22 KB while it holds
#    WiFi — opening the Air Mouse in those ten seconds was a boot loop.
#    So the free-RAM check has to come BEFORE the call into the controller.
python3 - <<'EOF' && ok "BLE checks free internal RAM before touching the controller" || bad "BLE goes to the controller without checking internal RAM first"
import re, sys
src = open("main/ble/hid_mouse.c", encoding="utf-8").read()
m = re.search(r"bool port_hid_start\(void\)\s*\{(.*?)\n\}", src, re.S)
if not m: print("  cannot find port_hid_start()"); sys.exit(1)
body = m.group(1)
guard = body.find("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)")
call  = body.find("esp_hid_gap_init(")
if guard < 0: print("  it never reads the free internal RAM"); sys.exit(1)
if call < 0:  print("  cannot find the call into the controller"); sys.exit(1)
if guard > call: print("  the check comes after the controller call, which is too late"); sys.exit(1)
if "return false" not in body[guard:call]: print("  it reads the RAM but does not refuse"); sys.exit(1)
EOF

echo "════ the mouse status text ════"
# 🚨 The label has to be written once when it is made — otherwise LVGL's default 'Text' stays
grep -A2 "s_state = lv_label_create" main/apps/app_mouse.c | grep -q "lv_label_set_text(s_state" \
  && ok "text is written when it is made" || bad "'Text' can be left behind"
# 🚨 Resetting the tracking value on entry is what makes the first update always write
grep -qE 's_prev_conn = -1;' main/apps/app_mouse.c \
  && ok "the tracking value is reset on entering the app" || bad "nothing is written when the state matches the last one"

echo "════ credentials ════"
# 🚨 Written into the code, flashing from a computer with no secrets.h loses the
#    badge its WiFi. The old example's "SSID here" overwrote perfectly good stored values.
# 🚨 Do not check by name — on 09-10 rec_upload.c moved from badge_creds_wifi to
#    badge_wifi_pick and a ✗ appeared against perfectly good code. Check the
#    meaning: (1) both places take it through a badge_ accessor, (2) nowhere
#    writes an SSID directly.
_creds_ok=1
for f in main/port_esp.c; do
  grep -qE "badge_(creds_wifi|wifi_pick)" "$f" || _creds_ok=0
done
# creds_seed in port_esp.c is where NVS is seeded, so it is the exception
grep -n "BADGE_WIFI_SSID" main/rec_upload.c main/apps/*.c >/dev/null 2>&1 && _creds_ok=0
[ "$_creds_ok" = 1 ] \
  && ok "credentials are read from the badge" || bad "credentials are written into the code"
! grep -qE '\.ssid = BADGE_WIFI_SSID' main/*.c \
  && ok "no written-in value goes into wifi_config" || bad "wifi_config is filled from a written-in value"
grep -qE '#define BADGE_WIFI_SSID +""' main/secrets.example.h \
  && ok "the example is empty (meaning: leave it alone)" || bad "the example holds a fake SSID"
# 🚨 Two examples and somebody copies the wrong one. The old secrets.h.example
#    carried the words "SSID here", and copying that and flashing overwrites the
#    perfectly good credentials stored in the badge's NVS (reproducing exactly
#    the accident fixed on 09-09).
[ ! -f main/secrets.h.example ] \
  && ok "there is only one credentials example" || bad "the old secrets.h.example is still there"

echo "════ the setup script's exit status ════"
# 🚨 `[ condition ] && echo` as the last line exits 1 when the condition is false.
#    A caller running set -e stops there.
tail -1 tools/setup.sh | grep -qx "exit 0" \
  && ok "the setup script exits 0" || bad "the setup script's exit status follows a condition"

echo "════ boards played by tilting ════"
# 🚨 The launcher counts only touch as activity. Games played by tilting had the screen go off mid-roll.
grep -q "lv_display_trigger_activity" main/apps/app_games.c \
  && ok "tilting counts as activity too" || bad "playing by tilt turns the screen off"
# 🚨 It must not go into boards driven by touch — touch already counts as activity.
#    Do not count occurrences (09-11: pinball adding one more call put a ✗
#    against perfectly good code). The meaning is "every function that reads the
#    tilt calls it, and every function that does not, does not".
python3 - <<'EOF' && ok "everywhere the tilt is read counts as activity" || bad "somewhere reads the tilt without counting activity"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
bad = []
for m in re.finditer(r"\n(?:static )?\w[\w \*]*?(\w+)\([^)]*\)\s*\n?\{", src):
    name = m.group(1)
    if name in ("tilt_is_input",): continue
    start = m.end()
    depth, i = 1, start
    while i < len(src) and depth:
        if src[i] == '{': depth += 1
        elif src[i] == '}': depth -= 1
        i += 1
    body = src[start:i]
    # The exception is written in the **code's own words**, not by name. A place
    # reached only from a tap has no reason to wake: touch already counted.
    if "only reached from a touch" in body: continue
    if "port_imu_accel(" in body and "tilt_is_input(" not in body:
        bad.append(name)
if bad:
    print("  reads the tilt without counting activity:", ", ".join(bad)); sys.exit(1)
EOF
# 🚨 Water is steered by tilting the boat too — nobody touches the screen and it went dark in 30 seconds (reported 09-09)
grep -q "lv_display_trigger_activity" main/apps/app_water.c \
  && ok "water stays awake while it is tilted" || bad "the screen goes off while water is being played"

echo "════ clearing the water image ════"
# 🚨 paint_img() dropped its memset and clears only what changed, which is what
#    took the frame from 80 ms to 54. The cost is that s_img now has to be
#    allocated zeroed: with no memset, anywhere nobody paints goes to the screen
#    exactly as it came from the allocator. Swapping the calloc back to a malloc
#    would put whatever PSRAM held on screen for the first frame, and nothing
#    would fail — it would just look wrong once, at startup, on a device nobody
#    is watching with a debugger.
python3 - <<'EOF' && ok "the water image is allocated zeroed, and nothing memsets it wholesale" || bad "s_img is not zeroed at allocation, or the wholesale memset came back"
import re, sys
src = open("main/apps/app_water.c", encoding="utf-8").read()
m = re.search(r"^\s*s_img\s*=\s*(\w+)\(", src, re.M)
if not m:
    print("  cannot find where s_img is allocated"); sys.exit(1)
if "calloc" not in m.group(1):
    print("  s_img comes from %s(), which does not zero it" % m.group(1)); sys.exit(1)
# the per-frame wholesale clear must not come back
if re.search(r"memset\(s_img \+ \(size_t\)s_img_y0", src):
    print("  the whole-band memset is back in paint_img()"); sys.exit(1)
EOF

echo "════ the frame yardstick ════"
# 🚨 Measuring only compute time is half the story — painting and pushing the screen are left out and it does not match the eye
# The **act of measuring** is checked, not the wording: both apps log the gap between frames
grep -qE "real interval" main/apps/app_water.c \
  && grep -qE "real interval" main/apps/orb.c \
  && ok "compute time and the real interval are measured together" || bad "only compute time is being measured"

echo "════ the orb textures ════"
# 🚨 Two separate scars here.
#    1. A picture made in code must never be written over a photograph someone
#       baked in with tools/make-orb-texture.py — hence the s_tex_bake guard.
#    2. Every kind needs its own bake. The Sun and Jupiter used to fall through
#       to bake_earth() and both came out as the Earth.
#    Do not pin this to the shape of the code — that check broke the moment the
#    if became a switch. Read the kinds out of the header and look for each one.
python3 - <<'EOF' && ok "every orb kind bakes its own, and only when nothing was baked in" || bad "an orb kind has no bake of its own, or a bake overwrites a baked-in photograph"
import re, sys
hdr = open("main/apps/orb.h", encoding="utf-8").read()
src = open("main/apps/orb.c", encoding="utf-8").read()
m = re.search(r"typedef enum \{([^}]*)\} orb_kind_t", hdr)
if not m: print("  cannot find orb_kind_t"); sys.exit(1)
kinds = [k.strip() for k in m.group(1).split(",") if k.strip()]
kinds = [k for k in kinds if k != "ORB_N"]
if len(kinds) < 2: print("  orb_kind_t looks wrong:", kinds); sys.exit(1)

blk = re.search(r"if \(s_tex_bake\)\s*\{(.*?)\n    \}", src, re.S)
if not blk: print("  the bake is not guarded by s_tex_bake"); sys.exit(1)
body = blk.group(1)
missing = []
for k in kinds:
    fn = "bake_" + k[len("ORB_"):].lower()
    if ("static void %s(void)" % fn) not in src: missing.append(fn + "() is not defined")
    elif (fn + "()") not in body:               missing.append(fn + "() is never reached")
if missing:
    print("  " + " · ".join(missing)); sys.exit(1)
EOF

echo "════ the orbs' attitude ════"
# 🚨 An axis that cannot lean sideways is a turntable, and Saturn's rings would lie flat forever
grep -q "s_lean" main/apps/orb.c \
  && ok "the axis leans sideways" || bad "the orbs are back to a turntable"
# 🚨 A leaning axis means the finger movement has to be unwound too — otherwise at 90 degrees across becomes down
grep -q "float bx =" main/apps/orb.c && grep -q "fabsf(bgy)" main/apps/orb.c \
  && ok "the drag is unwound by the lean" || bad "the drag direction goes wrong when it leans"
# 🚨 Saturn was taken out — the ring-drawing code and its photograph must not be left holding flash
! grep -qi "saturn" main/apps/orb.c main/apps/orb.h \
  && [ ! -f main/assets/orb_tex_saturn.h ] \
  && ok "no trace of Saturn is left" || bad "leftovers of Saturn remain"

echo "════ recording space ════"
# 🚨 Not taking exported recordings off the list means the space never comes back (09-08: all 12 exported, 3.9 minutes left)
grep -q "purge_sent" main/rec_store.c && grep -q "purge_sent();" main/rec_store.c \
  && ok "exported recordings come off the list" || bad "the space does not come back after exporting"
# 🚨 Shaking the list mid-recording has the microphone write into the wrong slot
grep -A2 "static void purge_sent(void)$" main/rec_store.c | grep -q "if (s_active) return;" \

echo "════ flash headroom ════"
# 🚨 A file still being written must not be read. A build was left running in the
#    background while the checks ran alongside, and reading a size that was not
#    finished lost 258 KB for nothing (09-08).
if [ -f build/badge_fw.bin ] && [ ! -f build/.ninja_lock ]; then
  SZ=$(stat -c%s build/badge_fw.bin)
  LEFT=$(( 6*1024*1024 - SZ ))
  echo "  app $(( SZ/1024 ))KB · $(( LEFT/1024 ))KB left"
  # 🚨 An orb photograph is 256 KB each. Adding more means reworking the partitions.
  [ "$LEFT" -gt 524288 ] && ok "at least 512KB free" \
    || bad "the app partition is tight — one orb photograph is 256KB"
fi

echo "════ the sdkconfig defaults ════"
# 🚨 A misspelt symbol in sdkconfig.defaults is not an error. Kconfig prints one
#    line during reconfigure and carries on with the default, so the setting you
#    thought you made was never made. CONFIG_ESP_COREDUMP_CHECKSUM_SHA sat there
#    for weeks doing nothing (the symbol is _SHA256). Every line here has to turn
#    up in the generated sdkconfig.
if [ -f sdkconfig ]; then
python3 - <<'EOF' && ok "every symbol in sdkconfig.defaults reached sdkconfig" || bad "a symbol in sdkconfig.defaults does not exist"
import re, sys
have = set(re.findall(r"^#?\s*(CONFIG_[A-Z0-9_]+)", open("sdkconfig", encoding="utf-8").read(), re.M))
bad = []
for ln in open("sdkconfig.defaults", encoding="utf-8"):
    m = re.match(r"^(CONFIG_[A-Z0-9_]+)=", ln.strip())
    if m and m.group(1) not in have:
        bad.append(m.group(1))
if bad:
    print("  not a real symbol:", " · ".join(bad)); sys.exit(1)
EOF
else
  echo "  · no sdkconfig yet — skipped (try again after idf.py build)"
fi

echo "════ the build list ════"
# 🚨 sync_cmake.py rewrites main/CMakeLists.txt from a list of folders it holds
#    internally, so a folder missing from that list is dropped silently and the
#    link fails with `undefined reference` a long way from the cause. Every .c
#    under main/ has to appear in the build list.
python3 - <<'EOF' && ok "every source under main/ is in the build list" || bad "a source is missing from the build list"
import os, sys
lst = open("main/CMakeLists.txt", encoding="utf-8").read()
missing = [os.path.relpath(os.path.join(r, f), "main").replace(os.sep, "/")
           for r, _, fs in os.walk("main") for f in fs if f.endswith(".c")]
missing = [m for m in missing if m not in lst]
if missing:
    print("  not in main/CMakeLists.txt:", " · ".join(sorted(missing))); sys.exit(1)
EOF

echo "════ timers (saving power once pinned the CPU) ════"
if grep -rn 'lv_timer_set_period(t' main/apps/*.c >/dev/null 2>&1; then
  bad "the period is changed inside a timer callback — lv_timer_handler goes round forever"
else
  ok "the period is not changed inside the callback (power is saved by skipping)"
fi
grep -q 'if (++skip' main/apps/app_mouse.c \
  && ok "it skips to save power when the screen is off" || bad "the screen-off power saving is missing"

echo "════ breakout levels ════"
# 🚨 The meaning is checked, not the name. The three below were actually walked into on 09-10.
python3 - <<'EOF' && ok "the level table stays inside what a person can play" || bad "the level table is out of range"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
tab = re.search(r"BRK_LV\[BRK_LEVELS\] = \{(.*?)\};", src, re.S).group(1)
rows = re.findall(r"\{([^}]*)\}", tab)
if len(rows) != 5: print("  there are not 5 levels"); sys.exit(1)
prev_hits = -1
for i, r in enumerate(rows):
    f = [x.strip().rstrip('f') for x in r.split(',')]
    spd0, spdmax, padh, nrows = float(f[0]), float(f[1]), float(f[2]), int(f[3])
    # A speed no human wrist can follow is luck, not difficulty
    if spdmax > 9.0: print(f"  L{i+1} cap {spdmax} — past 9.0 it becomes luck"); sys.exit(1)
    if spd0 > spdmax: print(f"  L{i+1} starts above its cap"); sys.exit(1)
    if padh < 14.0:   print(f"  L{i+1} the paddle is too short"); sys.exit(1)
    if nrows not in (3, 4): print(f"  L{i+1} the row count is not 3 or 4"); sys.exit(1)
EOF
# 🚨 An obstacle made as an arc invalidates a large rectangle whole on every angle change
grep -q "s_obs\[i\] = dot(" main/apps/app_games.c \
  && ! grep -q "s_obs\[i\] = lv_arc_create" main/apps/app_games.c \
  && ok "the obstacle is a moving dot (no arc is redrawn)" \
  || bad "an obstacle made as an arc pushes a large area again every step"
# 🚨 The first row of bricks must not encroach on the back and tilt buttons.
#    Do not write the position in — on 09-11 the buttons moved inward to clear
#    the wall (-196 → -174) and the check put a ✗ against good code.
#    **It is worked out from the buttons' position.**
python3 - <<'EOF' && ok "the first row of bricks does not hide the buttons" || bad "the top row is hidden behind the buttons"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
def num(pat, cast=int):
    m = re.search(pat, src)
    return cast(m.group(1)) if m else None
dy  = num(r"#define GBTN_IN_DY\s+\(?(-?\d+)\)?")
h   = num(r"add_back_xy[\s\S]{0,400}?lv_obj_set_size\(b, \d+, (\d+)\)")
top = num(r"#define BRK_TOP\s+(\d+)")
if None in (dy, h, top):
    print(f"  cannot find the positions (dy={dy} h={h} top={top})"); sys.exit(1)
btm = 233 + dy + h // 2          # the bottom edge of the buttons (screen coordinates)
if top < btm:
    print(f"  the first brick row y={top} is above the buttons' bottom edge y={btm}"); sys.exit(1)
EOF

# 🚨 A newly made dot has to be put in place before it starts. Without that LVGL
#    leaves it at (0,0) and the ball sits in the corner while "tap to start" is up.
python3 - <<'EOF' && ok "a newly made dot is put in place before it starts" || bad "a dot is left at (0,0)"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
bad = []
for m in re.finditer(r"\n(?:static )?[\w \*]*?\b(\w+)\([^)]*\)\s*\n?\{", src):
    start = m.end(); depth, i = 1, start
    while i < len(src) and depth:
        if src[i] == '{': depth += 1
        elif src[i] == '}': depth -= 1
        i += 1
    body = src[start:i]
    for v in set(re.findall(r"(\w+)\s*=\s*dot\(", body)):
        if f"put({v}" in body: continue
        # a helper called by the same function may do the putting (followed one level deep)
        helped = False
        for call in set(re.findall(r"\b(\w+)\(\s*\)\s*;", body)):
            h = re.search(r"\n(?:static )?[\w \*]*?\b%s\([^)]*\)\s*\n?\{" % call, src)
            if not h: continue
            st = h.end(); d, j = 1, st
            while j < len(src) and d:
                if src[j] == '{': d += 1
                elif src[j] == '}': d -= 1
                j += 1
            if f"put({v}" in src[st:j]: helped = True; break
        if not helped:
            bad.append(f"{v} in {m.group(1)}()")
if bad:
    print("  made and never put:", " · ".join(bad)); sys.exit(1)
EOF

# 🚨 On a round screen the width at that height has to be measured before
#    choosing a y, and the text has to be checked for sitting behind something
#    else. On 09-11 the "Games" title was behind the first button, and it took a
#    simulator screenshot to notice.
python3 - <<'EOF' && ok "the menu title is neither hidden behind a button nor clipped" || bad "the menu title is hidden or clipped"
import re, sys, math
src = open("main/apps/app_games.c", encoding="utf-8").read()
def num(pat):
    m = re.search(pat, src); return int(m.group(1)) if m else None
title = num(r"lv_obj_align\(t, LV_ALIGN_CENTER, 0, (-?\d+)\)")
btn0  = num(r"lv_obj_align\(b, LV_ALIGN_CENTER, 0, (-?\d+) \+ i \* \d+\)")
bw    = num(r"lv_obj_set_size\(b, (\d+), \d+\)")
bh    = num(r"lv_obj_set_size\(b, \d+, (\d+)\)")
if None in (title, btn0, bw, bh): print("  cannot find the positions"); sys.exit(1)
if title + 14 > btn0 - bh // 2:
    print(f"  the title ({title}) is behind the first button's top edge ({btn0 - bh//2})"); sys.exit(1)
half = math.sqrt(233**2 - min(abs(title) + 14, 232)**2)
if half * 2 < 120:
    print(f"  the title row (dy {title}) is only {half*2:.0f}px wide and is clipped"); sys.exit(1)
# the buttons have to fit inside the circle at their own heights too
for i in range(4):
    dy = abs(btn0 + i * 90) + bh // 2
    if (bw / 2) ** 2 + dy ** 2 > 233 ** 2:
        print(f"  button {i+1}'s corner falls outside the circle"); sys.exit(1)
EOF

echo "════ pinball ════"
python3 - <<'EOF' && ok "the flipper pivots sit on the wall and the gap is wider than the ball" || bad "the pinball layout is wrong"
import re, sys, math
src = open("main/apps/app_games.c", encoding="utf-8").read()
def d(name):
    m = re.search(r"#define %s\s+([\d.]+)f?" % name, src)
    return float(m.group(1)) if m else None
wall, px, py, L, br = d("PB_WALL"), d("PB_FLIP_PX"), d("PB_FLIP_PY"), d("PB_FLIP_L"), d("PB_BR")
if None in (wall, px, py, L, br): print("  cannot find the constants"); sys.exit(1)
# 🚨 A pivot off the wall lets the ball run down outside the flipper and the game does not work
off = abs(math.hypot(px, py) - wall)
if off > 10: print(f"  the flipper pivot is {off:.0f}px off the wall — a path opens outside it"); sys.exit(1)
# 🚨 A middle gap narrower than the ball never drains, and too wide cannot be defended
rest = float(re.search(r"pb_flip_t\)\{ CX - PB_FLIP_PX, CY \+ PB_FLIP_PY,\s*([\d.]+)f", src).group(1))
gap = 2 * (px - L * math.cos(math.radians(rest)))
if gap <= br * 2: print(f"  the gap {gap:.0f}px is narrower than the ball {br*2:.0f}px — it will never drain"); sys.exit(1)
if gap > 120:     print(f"  the gap {gap:.0f}px — too wide to defend"); sys.exit(1)
EOF

echo "════ does a long press survive the screen being rebuilt ════"
python3 tools/sim-hold-check.py && ok "a long press still lands during a rebuild" || bad "the long press dies (deleting the pressed object makes LVGL ignore input until release)"

echo "════ do the home pages still turn ════"
# 🚨 A gesture reaches only the pressed object. On 09-11 swapping the wallpaper
#    for an lv_obj_create() — clickable by default, unlike the image it replaced —
#    made the wallpaper swallow every swipe on the bare background and the pages
#    stopped turning. Nothing about the source looks wrong, so it is swiped for real.
python3 tools/sim-swipe-check.py >/dev/null 2>&1 && ok "swiping left and right turns the home pages" || bad "the home pages do not turn (something on home is eating the gesture)"

echo "════ do large arrays hold internal RAM permanently ════"
if [ ! -f build/badge_fw.map ]; then
  echo "  · not built yet — skipped (try again after idf.py build)"
else
BSS=$(python3 tools/bss-size.py)
echo "  our code's .bss total ${BSS} bytes"
[ "$BSS" -lt 12000 ] && ok "under 12KB of permanent internal RAM" || bad "too much permanent internal RAM (it has to move to PSRAM)"
fi

echo
[ $FAIL -eq 0 ] && echo "→ all passed" || { echo "→ something has come back"; exit 1; }
