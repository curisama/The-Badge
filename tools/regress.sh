#!/usr/bin/env bash
# 지금까지 물렸던 것들이 되살아났는지 소스에서 확인한다.
#
# 🚨 같은 실수를 두 번 한 적이 있다(그림보다 메모리를 먼저 놓기 — 달·지구에서
#    고치고 물에서 반복). 고친 자리를 사람이 기억하는 걸로는 안 된다.
#    고침의 '흔적'을 검사로 굳혀둔다.
set -u
cd "$(dirname "$0")/.."
FAIL=0
ok()  { printf '  ✓ %s\n' "$1"; }
bad() { printf '  ✗ %s\n' "$1"; FAIL=1; }
has() { grep -qF "$2" "$1" 2>/dev/null; }

echo "════ 그리기 ════"
has main/display.c ".use_psram = false" \
  && ok "그리기 버퍼가 내부 RAM 에 있다 (PSRAM 이면 전송마다 임시버퍼를 잡아 13,943번 실패했다)" \
  || bad "그리기 버퍼가 PSRAM 으로 돌아갔다"
# 🚨 예전엔 ".buffer_height = 16" 을 그대로 찾았다. 그 값은 성능을 재가며
#    바꾸는 값이다(0909 에 근거를 대고 24 로 올렸다). 숫자에 매인 검사가
#    멀쩡한 변경을 틀렸다고 했다 — 검사는 이름이 아니라 뜻을 봐야 한다.
#    지켜야 할 뜻은 "화면 전체를 한 번에 담지 않는다"(내부 RAM 이 터진다).
BH=$(grep -oE "\.buffer_height = [0-9]+" main/display.c | grep -oE "[0-9]+")
[ -n "$BH" ] && [ "$BH" -ge 8 ] && [ "$BH" -le 64 ] \
  && ok "한 번에 ${BH}줄만 보낸다 (전체를 한꺼번에 안 담는다)" \
  || bad "띠 높이가 이상하다 (지금 '${BH:-없음}')"
sed -n "/static esp_err_t panel_cmd/,/^}/p" main/display.c | grep -q port_lock \
  && ok "패널 명령이 락을 잡는다 (밝기 바꾸기가 그리기와 SPI 를 다퉜다)" \
  || bad "panel_cmd 가 락을 안 잡는다"

echo "════ 무선 ════"
has main/ble/hid_mouse.c "esp_hid_gap_deinit()" \
  && ok "BLE 를 내릴 때 HID GAP 도 내린다 (안 내리면 다시는 안 올라온다)" \
  || bad "esp_hid_gap_deinit 이 빠졌다"

echo "════ 소리 ════"
has main/port_esp.c "tone_muted" \
  && ok "소리를 끄면 코덱을 안 잡는다 (예전엔 무음을 밀며 전기를 썼다)" \
  || bad "음소거인데 코덱을 잡는다"
grep -q "s_tone_held ? 20000000" main/port_esp.c \
  && ok "붙잡아도 20초 조용하면 놓는다" || bad "코덱을 무한정 붙잡는다"

echo "════ 기록 ════"
has main/port_esp.c "JRN_BREAK" \
  && ok "충전 뒤 다시 뽑아도 일지를 안 지운다 (경계만 남기고 이어 쓴다)" \
  || bad "일지가 뽑을 때 지워진다"
has main/port_esp.c "잔량이 %d%%→%d%% 로 회복" \
  && ok "부하로 눌린 잔량을 두 번 세지 않는다" || bad "잔량 이중계상이 돌아왔다"

echo "════ 나가는 길 (그림보다 메모리를 먼저 놓지 않기) ════"
grep -A6 "^void orb_stop" main/apps/orb.c | grep -q "lv_obj_delete(s_canvas)" \
  && ok "달·지구: 그림을 먼저 지운다" || bad "orb_stop 이 메모리를 먼저 놓는다"
grep -A8 "^void water_stop" main/apps/app_water.c | grep -q "lv_obj_delete(s_field)" \
  && ok "물: 그림을 먼저 지운다" || bad "water_stop 이 메모리를 먼저 놓는다"
grep -A3 "^static void draw_cb" main/apps/app_water.c | grep -q "if (!s_x" \
  && ok "물: 놓은 뒤엔 그리지 않는다" || bad "물 draw_cb 에 안전문이 없다"

echo "════ 축·방향 (실기에서 여러 번 틀렸다) ════"
grep -q "s_mvx -= gy" main/apps/app_games.c && grep -q "s_mvy -= gx" main/apps/app_games.c \
  && ok "구슬: IMU 축이 화면과 90도 돌아간 걸 반영" || bad "구슬 축이 되돌아갔다"
grep -qE "yy = -\(float\)dy / (ORB_R|s_R)" main/apps/orb.c \
  && ok "천체: 화면 위가 북쪽" || bad "천체 남북이 뒤집혔다"
grep -q "float lat = gy;" main/apps/app_games.c \
  && ok "기울기 판: 기울인 쪽으로 간다" || bad "기울기 판 좌우가 반대다"

echo "════ 에어마우스 ════"
LAST=$(grep -n 'air_paint()' main/apps/app_mouse.c | tail -1 | cut -d: -f1)
BTN=$(grep -n 's_air_l = mk_click_btn' main/apps/app_mouse.c | cut -d: -f1)
[ -n "$LAST" ] && [ -n "$BTN" ] && [ "$LAST" -gt "$BTN" ] \
  && ok "단추를 다 만든 뒤에 보이고 감춘다 (먼저 부르면 숨은 채로 남는다)" \
  || bad "air_paint 를 단추보다 먼저 부른다 — 좌우 단추가 안 보인다"

echo "════ 쪽 넘기기 ════"
grep -q 'build_home();' main/launcher.c && grep -A6 's_swiped = true;' main/launcher.c | grep -q 'build_home' \
  && ok "쪽이 바뀌면 홈을 새로 짓는다 (다시 띄우기만 하면 화면이 그대로다)" \
  || bad "쪽만 바꾸고 홈을 안 짓는다"

grep -qE 's_prev_p = -2;' main/launcher.c \
  && ok "홈을 새로 지으면 배터리 숫자를 반드시 한 번 쓴다 (안 쓰면 LVGL 기본 글자 Text 가 남는다)" \
  || bad "새 홈에서 배터리 라벨이 기본 글자로 남을 수 있다"
grep -B2 's_batt_timer = lv_timer_create' main/launcher.c | grep -q 'lv_timer_delete' \
  && ok "홈을 새로 지을 때 옛 배터리 타이머를 지운다 (안 지우면 쪽 넘길 때마다 쌓인다)" \
  || bad "홈을 새로 지을 때 타이머가 쌓인다"

echo "════ 어디서든 도는가 ════"
# 🚨 -Wl,--start-group 은 GNU ld 것이다. 애플 링커는 모르고 링크가 죽는다
grep -q 'sys.platform == "darwin"' sim/build.py \
  && ok "맥 링커를 따로 본다" || bad "맥에서 링크가 죽는다(--start-group)"
# 🚨 배지가 잡히는 이름이 기계마다 다르다 — 리눅스만 적어두면 다른 데서 헤맨다
grep -q "cu.usbmodem" tools/setup.sh \
  && ok "포트 안내가 기계에 맞춰 나온다" || bad "포트 안내가 리눅스 것뿐이다"
# 🚨 홈서버 경로가 박히면 다른 기계에선 도구가 통째로 안 돈다
! grep -q "/home/" tools/mkassets.py \
  && ok "자산 도구에 박힌 경로가 없다" || bad "mkassets.py 에 홈서버 경로가 박혀 있다"
# 🚨 시뮬용 LVGL 을 managed_components 에 넣으면 나중에 idf.py 가 거부한다
! grep -q "managed_components/lvgl__lvgl$" tools/setup.sh \
  && ok "시뮬용 LVGL 은 sim/lvgl 에 둔다" || bad "시뮬용 LVGL 이 managed_components 를 더럽힌다"
# 🚨 윈도우에서도 서야 한다 — 빌드 몰이는 파이썬, POSIX 다리는 win_compat.h
[ -f sim/build.py ] && [ -f sim/win_compat.h ] \
  && ok "윈도우에서도 시뮬이 선다" || bad "윈도우 지원이 빠졌다"
# 🚨 여러 줄 로그를 한 줄로 이어 붙여 봐야 오탐이 없다

echo "════ 인수인계 문서 ════"
# 🚨 다음 사람과 다음 LLM 이 읽는 유일한 정본이다. 없으면 아무도 못 따라온다.
[ -f README.md ] && ok "README 가 있다" || bad "README.md 가 없다"
[ -f NOTICE ]    && ok "제3자 고지가 있다" || bad "NOTICE 가 없다"
[ -f LICENSE ]   && ok "라이선스가 있다" || bad "LICENSE 가 없다"
# 코드가 문서보다 한참 앞서 나갔으면 알려준다(고치라는 뜻이지 실패는 아니다)
if [ -f docs/HANDOFF.md ]; then
  NEW=$(git log --oneline -20 --format=%H -- main/ tools/ 2>/dev/null | head -1)
  DOC=$(git log --oneline -1 --format=%H -- docs/HANDOFF.md 2>/dev/null)
  AHEAD=$(git rev-list --count "$DOC..$NEW" -- main/ tools/ 2>/dev/null || echo 0)
  [ "${AHEAD:-0}" -gt 3 ] \
    && echo "  ⚠ 코드가 인수인계 문서보다 커밋 ${AHEAD}개 앞서 있다 — 고칠 게 없는지 볼 것"
fi

echo "════ 벤치가 꺼져 있나 ════"
# 🚨 켠 채로 구우면 배지가 부팅마다 스스로 앱을 열어 사람 조작을 뺏는다
grep -qE "^add_compile_definitions\(BADGE_APPBENCH" CMakeLists.txt \
  && bad "앱 벤치가 켜져 있다" || ok "앱 벤치는 꺼져 있다"

echo "════ 검증 통로가 밖에 있나 ════"
# 🚨 함수가 다른 함수 몸통 안에 박히면 GCC 중첩함수 확장으로 빌드는 되는데
#    밖에서 못 부른다. 예전에 games_debug_play_bricks 가 그렇게 숨어 있었다.
awk '/^static void brk_touch/,/^}/' main/apps/app_games.c | grep -q "games_debug" \
  && bad "검증 함수가 brk_touch 안에 박혀 있다" || ok "검증 함수가 파일 바깥에 있다"

echo "════ IMU 깨우기 ════"
# 🚨 재웠다 깬 직후 값은 못 믿는다 — 그걸로 "수평" 기준을 잡으면 판이 끝에 붙는다
grep -q "imu_settled" main/port_esp.c \
  && ok "깬 직후엔 아직 모른다고 답한다" || bad "깨자마자 읽은 값을 그대로 준다"
# 🚨 물은 0909 에 '열 때 자세를 수평으로 박는' 방식을 버리고 진짜 중력을
#    쓰게 됐다. 그래서 기준을 잡는 일 자체가 없어졌다 — 예전 검사는 사라진
#    코드를 찾고 있었다. 이제 지켜야 할 뜻은 "IMU 를 못 믿을 때 그 값을
#    그대로 쓰지 않는다" 이다.
grep -q "bool have = port_imu_accel3" main/apps/app_water.c \
  && grep -q "if (!have)" main/apps/app_water.c \
  && ok "물은 IMU 를 못 믿을 때 대신 값을 쓴다" || bad "물이 못 믿을 값을 그대로 쓴다"

echo "════ 마우스 상태 글자 ════"
# 🚨 라벨을 만들 때 한 번은 써야 한다 — 안 쓰면 LVGL 기본 글자 Text 가 남는다
grep -A2 "s_state = lv_label_create" main/apps/app_mouse.c | grep -q "lv_label_set_text(s_state" \
  && ok "만들 때 글자를 넣는다" || bad "Text 가 남을 수 있다"
# 🚨 들어갈 때 추적값을 되돌려야 첫 갱신이 반드시 쓴다
grep -qE 's_prev_conn = -1;' main/apps/app_mouse.c \
  && ok "앱에 들어갈 때 추적값을 되돌린다" || bad "지난 상태와 같으면 안 쓴다"

echo "════ 접속정보 ════"
# 🚨 코드에 박아 두면 secrets.h 가 없는 컴퓨터에서 구웠을 때 배지가 WiFi 를
#    잃는다. 예전 예제의 "여기에 SSID" 는 저장된 멀쩡한 값까지 덮어썼다.
# 🚨 이름으로 검사하지 마라 — 0910 에 rec_upload.c 가 badge_creds_wifi 에서
#    badge_wifi_pick 으로 갈아탔더니 멀쩡한 코드에 ✗ 가 떴다. 뜻을 본다:
#    (1) 두 곳 다 badge_ 통로로 받아올 것, (2) 어디서도 SSID 를 직접 안 쓸 것.
_creds_ok=1
for f in main/port_esp.c; do
  grep -qE "badge_(creds_wifi|wifi_pick)" "$f" || _creds_ok=0
done
# port_esp.c 의 creds_seed 는 NVS 에 씨 뿌리는 자리라 예외로 둔다
grep -n "BADGE_WIFI_SSID" main/rec_upload.c main/apps/*.c >/dev/null 2>&1 && _creds_ok=0
[ "$_creds_ok" = 1 ] \
  && ok "접속정보를 배지에서 읽는다" || bad "접속정보가 코드에 박혀 있다"
! grep -qE '\.ssid = BADGE_WIFI_SSID' main/*.c \
  && ok "wifi_config 에 박은 값을 안 넣는다" || bad "박은 값으로 wifi_config 를 채운다"
grep -qE '#define BADGE_WIFI_SSID +""' main/secrets.example.h \
  && ok "예제가 비어 있다 (건드리지 마라는 뜻)" || bad "예제에 가짜 SSID 가 있다"
# 🚨 예제가 둘이면 사람이 엉뚱한 것을 베낀다. 옛 secrets.h.example 엔
#    "여기에 SSID" 라는 글자가 있어서, 그걸 베껴 구우면 배지 NVS 에 저장된
#    멀쩡한 접속정보를 덮어쓴다(0909 에 고친 사고가 그대로 재현된다).
[ ! -f main/secrets.h.example ] \
  && ok "접속정보 예제가 하나뿐이다" || bad "옛 secrets.h.example 이 남아 있다"

echo "════ 설치 스크립트 끝값 ════"
# 🚨 `[ 조건 ] && echo` 가 마지막 줄이면 조건이 거짓일 때 1 로 끝난다.
#    부르는 쪽이 set -e 면 거기서 중단된다.
tail -1 tools/setup.sh | grep -qx "exit 0" \
  && ok "설치 스크립트가 0 으로 끝난다" || bad "설치 스크립트 끝값이 조건에 딸려간다"

echo "════ 기울여 노는 판 ════"
# 🚨 런처는 터치만 조작으로 친다. 기울여 노는 게임은 굴리는 중에 화면이 꺼졌다.
grep -q "lv_display_trigger_activity" main/apps/app_games.c \
  && ok "기울이는 것도 조작으로 센다" || bad "기울여 놀면 화면이 꺼진다"
# 🚨 터치로 모는 판엔 넣으면 안 된다 — 터치가 이미 조작으로 센다.
#    개수로 세지 마라(0911: 핀볼이 하나 더 부르자 멀쩡한 코드에 ✗ 가 떴다).
#    뜻은 "기울기를 읽는 함수는 전부 부르고, 안 읽는 함수는 안 부른다" 이다.
python3 - <<'EOF' && ok "기울기를 읽는 곳마다 조작으로 센다" || bad "기울기를 읽는데 조작으로 안 세는 곳이 있다"
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
    # 예외는 이름이 아니라 **코드에 적힌 뜻**으로 둔다. 탭에서만 오는 자리는
    # 터치가 이미 조작으로 세어졌으므로 깨울 이유가 없다.
    if "only reached from a touch" in body: continue
    if "port_imu_accel(" in body and "tilt_is_input(" not in body:
        bad.append(name)
if bad:
    print("  기울기를 읽는데 조작으로 안 세는 함수:", ", ".join(bad)); sys.exit(1)
EOF
# 🚨 물도 배를 기울여 몬다 — 화면을 안 만져서 30초 만에 꺼졌다(0909 제보)
grep -q "lv_display_trigger_activity" main/apps/app_water.c \
  && ok "물도 기울이면 깨어 있다" || bad "물 갖고 노는 중에 화면이 꺼진다"

echo "════ 프레임 눈금 ════"
# 🚨 계산 시간만 재면 반쪽이다 — 칠하기와 화면 보내기가 빠져 눈과 안 맞는다
# 문구가 아니라 **재는 행위**를 본다: 두 앱 다 프레임 사이 간격을 로그에 낸다
grep -qE "real interval|진짜 간격" main/apps/app_water.c \
  && grep -qE "real interval|진짜 간격" main/apps/orb.c \
  && ok "계산 시간과 진짜 간격을 같이 잰다" || bad "계산 시간만 재고 있다"

echo "════ 천체 사진 ════"
# 🚨 구워둔 진짜 사진 위에 코드로 만든 그림을 덮어쓰면 안 된다
#    (태양·목성이 전부 지구로 나왔다. "잡았다" 와 "구워야 한다" 는 다른 말이다)
grep -q "s_tex_bake" main/apps/orb.c \
  && grep -q "if (s_tex_bake) { if (kind == ORB_MOON)" main/apps/orb.c \
  && ok "진짜 사진 위에 안 덮어쓴다" || bad "사진 위에 만든 그림을 덮어쓴다"

echo "════ 천체 자세 ════"
# 🚨 축이 옆으로 기울지 못하면 회전판이라 토성 고리가 영영 가로로만 눕는다
grep -q "s_lean" main/apps/orb.c \
  && ok "축이 옆으로 기운다" || bad "천체가 회전판으로 되돌아갔다"
# 🚨 축이 기울면 손가락 움직임도 되돌려야 한다 — 안 하면 90도에서 가로가 세로가 된다
grep -q "float bx =" main/apps/orb.c && grep -q "fabsf(bgy)" main/apps/orb.c \
  && ok "기운 만큼 손끌기를 되돌린다" || bad "기울면 손끌기 방향이 어긋난다"
# 🚨 토성을 뺐다 — 고리 그리는 코드와 사진이 남아 플래시를 물면 안 된다
! grep -qi "saturn" main/apps/orb.c main/apps/orb.h \
  && [ ! -f main/assets/orb_tex_saturn.h ] \
  && ok "토성 흔적이 안 남았다" || bad "토성 찌꺼기가 남았다"

echo "════ 녹음 자리 ════"
# 🚨 올린 녹음을 목록에서 빼지 않으면 자리가 영영 안 돌아온다(0908: 12건 다 올렸는데 3.9분)
grep -q "purge_sent" main/rec_store.c && grep -q "purge_sent();" main/rec_store.c \
  && ok "올린 녹음은 목록에서 뺀다" || bad "올려도 자리가 안 돌아온다"
# 🚨 녹음 중에 목록을 흔들면 마이크가 엉뚱한 칸에 쓴다
grep -A2 "static void purge_sent(void)$" main/rec_store.c | grep -q "if (s_active) return;" \

echo "════ 플래시 여유 ════"
# 🚨 짓는 중인 파일을 읽으면 안 된다. 뒤에서 빌드를 돌려놓고 검사를 같이
#    돌렸다가, 아직 다 안 쓴 크기를 읽고 258KB 를 헛되이 놓쳤다(0908).
if [ -f build/badge_fw.bin ] && [ ! -f build/.ninja_lock ]; then
  SZ=$(stat -c%s build/badge_fw.bin)
  LEFT=$(( 6*1024*1024 - SZ ))
  echo "  앱 $(( SZ/1024 ))KB · 남은 자리 $(( LEFT/1024 ))KB"
  # 🚨 천체 사진이 하나당 256KB 다. 더 넣으려면 파티션을 손봐야 한다.
  [ "$LEFT" -gt 524288 ] && ok "여유 512KB 이상" \
    || bad "앱 파티션이 빠듯하다 — 천체 사진 하나가 256KB 다"
fi

echo "════ 빌드 목록 ════"
grep -q 'fonts/badge_kr' main/CMakeLists.txt \

echo "════ 타이머 (절전하려다 CPU 를 물고 늘어진 적이 있다) ════"
if grep -rn 'lv_timer_set_period(t' main/apps/*.c >/dev/null 2>&1; then
  bad "타이머 콜백 안에서 주기를 바꾼다 — lv_timer_handler 가 무한히 다시 돈다"
else
  ok "콜백 안에서 주기를 안 바꾼다 (건너뛰기로 절전한다)"
fi
grep -q 'if (++skip' main/apps/app_mouse.c \
  && ok "화면 꺼지면 건너뛰어 절전한다" || bad "화면 꺼짐 절전이 빠졌다"

echo "════ 벽돌깨기 단계 ════"
# 🚨 이름이 아니라 뜻을 본다. 아래 셋은 0910 에 실제로 밟은 것들이다.
python3 - <<'EOF' && ok "단계 표가 사람이 칠 수 있는 범위 안이다" || bad "단계 표가 범위를 벗어났다"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
tab = re.search(r"BRK_LV\[BRK_LEVELS\] = \{(.*?)\};", src, re.S).group(1)
rows = re.findall(r"\{([^}]*)\}", tab)
if len(rows) != 5: print("  단계가 5개가 아니다"); sys.exit(1)
prev_hits = -1
for i, r in enumerate(rows):
    f = [x.strip().rstrip('f') for x in r.split(',')]
    spd0, spdmax, padh, nrows = float(f[0]), float(f[1]), float(f[2]), int(f[3])
    # 사람 손목이 못 따라가는 속도는 난이도가 아니라 운이다
    if spdmax > 9.0: print(f"  L{i+1} 상한 {spdmax} — 9.0 을 넘으면 운이 된다"); sys.exit(1)
    if spd0 > spdmax: print(f"  L{i+1} 시작이 상한보다 크다"); sys.exit(1)
    if padh < 14.0:   print(f"  L{i+1} 판이 너무 짧다"); sys.exit(1)
    if nrows not in (3, 4): print(f"  L{i+1} 줄 수가 3~4 가 아니다"); sys.exit(1)
EOF
# 🚨 방해물을 호로 만들면 각도를 바꿀 때마다 큰 네모가 통째로 무효화된다
grep -q "s_obs\[i\] = dot(" main/apps/app_games.c \
  && ! grep -q "s_obs\[i\] = lv_arc_create" main/apps/app_games.c \
  && ok "방해물은 옮기는 구슬이다 (호를 다시 그리지 않는다)" \
  || bad "방해물을 호로 만들면 걸음마다 큰 영역을 다시 민다"
# 🚨 벽돌 첫 줄이 뒤로·기울기 단추를 침범하면 안 된다.
#    자리값을 박아 두지 마라 — 0911 에 단추가 벽을 피해 안으로 들어오면서
#    (-196 → -174) 검사가 멀쩡한 코드에 ✗ 를 냈다. **단추 자리에서 계산한다.**
python3 - <<'EOF' && ok "벽돌 첫 줄이 단추를 안 가린다" || bad "맨 윗줄이 단추에 가린다"
import re, sys
src = open("main/apps/app_games.c", encoding="utf-8").read()
def num(pat, cast=int):
    m = re.search(pat, src)
    return cast(m.group(1)) if m else None
dy  = num(r"#define GBTN_IN_DY\s+\(?(-?\d+)\)?")
h   = num(r"add_back_xy[\s\S]{0,400}?lv_obj_set_size\(b, \d+, (\d+)\)")
top = num(r"#define BRK_TOP\s+(\d+)")
if None in (dy, h, top):
    print(f"  자리값을 못 찾겠다 (dy={dy} h={h} top={top})"); sys.exit(1)
btm = 233 + dy + h // 2          # 단추 아래 끝 (화면 좌표)
if top < btm:
    print(f"  벽돌 첫 줄 y={top} 이 단추 아래끝 y={btm} 보다 위다"); sys.exit(1)
EOF

# 🚨 새로 만든 알은 반드시 자리에 놓고 시작해야 한다. 안 놓으면 LVGL 이
#    (0,0) 에 두고, "tap to start" 가 떠 있는 동안 공이 구석에 앉아 있는다.
python3 - <<'EOF' && ok "새로 만든 알을 자리에 놓고 시작한다" || bad "알이 (0,0) 에 남는다"
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
        # 같은 함수가 부르는 도우미가 놓아줘도 된다 (한 겹만 따라간다)
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
            bad.append(f"{m.group(1)}() 의 {v}")
if bad:
    print("  만들고 안 놓은 알:", " · ".join(bad)); sys.exit(1)
EOF

# 🚨 둥근 화면에선 y 를 정할 때 그 자리의 폭부터 재야 하고, 글자가 다른 것
#    뒤에 깔리지 않는지도 봐야 한다. 0911 에 "Games" 제목이 첫 단추 뒤로
#    깔린 걸 시뮬 그림 보고서야 알았다.
python3 - <<'EOF' && ok "메뉴 제목이 단추에 안 깔리고 안 잘린다" || bad "메뉴 제목이 가리거나 잘린다"
import re, sys, math
src = open("main/apps/app_games.c", encoding="utf-8").read()
def num(pat):
    m = re.search(pat, src); return int(m.group(1)) if m else None
title = num(r"lv_obj_align\(t, LV_ALIGN_CENTER, 0, (-?\d+)\)")
btn0  = num(r"lv_obj_align\(b, LV_ALIGN_CENTER, 0, (-?\d+) \+ i \* \d+\)")
bw    = num(r"lv_obj_set_size\(b, (\d+), \d+\)")
bh    = num(r"lv_obj_set_size\(b, \d+, (\d+)\)")
if None in (title, btn0, bw, bh): print("  자리값을 못 찾겠다"); sys.exit(1)
if title + 14 > btn0 - bh // 2:
    print(f"  제목({title})이 첫 단추 위끝({btn0 - bh//2}) 에 깔린다"); sys.exit(1)
half = math.sqrt(233**2 - min(abs(title) + 14, 232)**2)
if half * 2 < 120:
    print(f"  제목 자리(dy {title})의 폭이 {half*2:.0f}px 뿐이라 잘린다"); sys.exit(1)
# 단추도 그 자리에서 원 안에 들어가야 한다
for i in range(4):
    dy = abs(btn0 + i * 90) + bh // 2
    if (bw / 2) ** 2 + dy ** 2 > 233 ** 2:
        print(f"  {i+1}번 단추 모서리가 원 밖으로 나간다"); sys.exit(1)
EOF

echo "════ 핀볼 ════"
python3 - <<'EOF' && ok "날개 축이 벽에 붙어 있고 구멍이 공보다 크다" || bad "핀볼 배치가 틀렸다"
import re, sys, math
src = open("main/apps/app_games.c", encoding="utf-8").read()
def d(name):
    m = re.search(r"#define %s\s+([\d.]+)f?" % name, src)
    return float(m.group(1)) if m else None
wall, px, py, L, br = d("PB_WALL"), d("PB_FLIP_PX"), d("PB_FLIP_PY"), d("PB_FLIP_L"), d("PB_BR")
if None in (wall, px, py, L, br): print("  상수를 못 찾겠다"); sys.exit(1)
# 🚨 축이 벽에서 떨어져 있으면 공이 날개 바깥으로 흘러내려 게임이 성립 안 한다
off = abs(math.hypot(px, py) - wall)
if off > 10: print(f"  날개 축이 벽에서 {off:.0f}px 떨어져 있다 — 바깥길이 생긴다"); sys.exit(1)
# 🚨 가운데 구멍이 공보다 좁으면 영영 안 빠지고, 너무 넓으면 못 지킨다
rest = float(re.search(r"pb_flip_t\)\{ CX - PB_FLIP_PX, CY \+ PB_FLIP_PY,\s*([\d.]+)f", src).group(1))
gap = 2 * (px - L * math.cos(math.radians(rest)))
if gap <= br * 2: print(f"  구멍 {gap:.0f}px 이 공 {br*2:.0f}px 보다 좁다 — 공이 안 빠진다"); sys.exit(1)
if gap > 120:     print(f"  구멍 {gap:.0f}px — 너무 넓어 지킬 수가 없다"); sys.exit(1)
EOF

echo "════ 길게 누르기가 화면 다시 짓기에 안 죽나 ════"
python3 tools/sim-hold-check.py && ok "다시 짓는 중에도 길게 누르기가 걸린다" || bad "길게 누르기가 죽는다 (누른 객체를 지우면 LVGL 이 손 뗄 때까지 무시한다)"

echo "════ 큰 배열이 내부 RAM 을 상시 물지 않나 ════"
if [ ! -f build/badge_fw.map ]; then
  echo "  · 아직 안 구웠다 — 건너뜀 (idf.py build 뒤에 다시)"
else
BSS=$(python3 tools/bss-size.py)
echo "  우리 코드 .bss 합계 ${BSS} 바이트"
[ "$BSS" -lt 12000 ] && ok "상시 내부 RAM 12KB 미만" || bad "상시 내부 RAM 이 너무 크다 (PSRAM 으로 옮겨야 한다)"
fi

echo
[ $FAIL -eq 0 ] && echo "→ 전부 통과" || { echo "→ 되살아난 것이 있다"; exit 1; }
