#!/bin/bash
# 배지가 왜 재부팅됐는지 한 번에 뽑는다.
#
#   ./tools/badge-diag.sh          기록 읽기 (아무것도 안 지운다)
#   ./tools/badge-diag.sh --clear  기록 비우기 (사람이 명시적으로 할 때만)
#
# 🚨 기록은 스스로 절대 안 지워진다. 읽으려면 꽂아야 하는데 꽂는 순간
#    지워지면 아무 소용이 없다(0906 에 배터리 일지를 그렇게 날렸다).
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-/dev/ttyACM0}

# 🚨 윈도우의 COMx 는 파일이 아니다. [ -e COM6 ] 은 배지가 꽂혀 있어도
#    늘 거짓이라, 여기서 "보드가 안 보인다" 며 끝나 버렸다(0909).
#    COMx 면 존재 검사를 건너뛰고 파이썬이 열어보며 판단하게 둔다.
port_missing() {
    case "$1" in
        COM[0-9]*|com[0-9]*) return 1 ;;
        *) [ -e "$1" ] && return 1 || return 0 ;;
    esac
}

if port_missing "$PORT"; then
    echo "✗ 보드가 안 보인다 ($PORT)"
    echo "  윈도우:  PORT=COM6 ./tools/badge-diag.sh   (장치관리자에서 번호 확인)"
    echo "  WSL   :  usbipd attach --wsl --busid 1-1"
    exit 1
fi

echo "════════ 1. 부팅 기록 (리셋 사유 · 가동시간 · 하던 일) ════════"
timeout 25 python3 - "$PORT" <<'PYEOF'
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 15
# 태그로 거른다. 문구를 바꿔도 안 새게.
TAGS = (') rst:', ') badge:', ') batt:', 'rst:0x', 'Light sleep')
while time.time() < end:
    t = s.readline().decode('utf-8', 'replace').rstrip()
    if t and any(k in t for k in TAGS):
        print(t)
PYEOF

echo
echo "════════ 2. 코어덤프 (패닉이면 죽은 자리가 나온다) ════════"
if [ ! -f build/badge_fw.elf ]; then
    echo "  (build/badge_fw.elf 없음 — idf.py build 먼저)"
elif ! command -v idf.py >/dev/null 2>&1; then
    echo "  (IDF 환경 아님 —  . ~/esp/esp-idf/export.sh  먼저)"
else
    OUT=$(idf.py -p "$PORT" coredump-info 2>&1)
    # 빈 파티션은 0xff 로 차 있어서 "version 0xffff is not supported" 로 나온다
    if echo "$OUT" | grep -qiE 'No core dump|not found|is empty|0xffff\" is not supported|Incorrect size of core dump'; then
        echo "  덤프 없음 = 패닉으로 죽은 적은 없다"
        echo "  → 부팅 기록이 브라운아웃[9]/전원글리치[14]면 전원 문제,"
        echo "    워치독[5~7]이면 어딘가 CPU 를 안 놓은 것이다"
    else
        HIT=$(echo "$OUT" | grep -A40 -iE 'crashed task|PC:|backtrace|panic|Exception' | head -50)
        if [ -n "$HIT" ]; then echo "$HIT"; else
            echo "  덤프를 읽었지만 익숙한 항목이 없다. 전체 출력:"
            echo "$OUT" | tail -25
        fi
    fi
fi

if [ "${1:-}" = "--clear" ]; then
    echo
    echo "════════ 기록 비우기 ════════"
    idf.py -p "$PORT" coredump-erase 2>&1 | tail -2
    echo "  ※ 부팅 기록(NVS)은 다음 12번까지 자동으로 밀려나므로 그대로 둔다"
fi

echo
echo "════════ 읽는 법 ════════"
cat <<'HELP'
  '화면켬' 에서 패닉/워치독이 반복  → 깨우는 경로가 범인
  브라운아웃[9] · 전원글리치[14]    → 전압 문제 (배터리 잔량 같이 볼 것)
  직전 가동시간이 늘 짧다           → 부팅 직후 죽는 것
  코어덤프가 있다                   → 그게 정답이다. 파일·줄까지 나온다
HELP
