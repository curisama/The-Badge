#!/bin/bash
# 배지가 지금 어떤 상태인지 알려준다.
# "다운로드 모드 들어갔나"보다 중요한 건 "지금 구울 수 있나"다 — 그걸 본다.
PORT=${1:-/dev/ttyACM0}

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
    echo "✗ 보드가 안 보인다 ($PORT 없음)"
    echo "  → USB 케이블 확인."
    echo "     윈도우면:  ./tools/badge-mode.sh COM6"
    echo "     WSL 이면:  usbipd attach --wsl --busid 1-1"
    exit 1
fi

# ROM 이 부팅 때 찍는 줄을 읽는다. boot:0x0 = 다운로드, boot:0xb = 평소 부팅.
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
    echo "✓ 다운로드 모드 (ROM 이 명령을 기다리는 중)"
    echo "$BANNER" | grep -i 'boot:' | head -1
    exit 0
}
echo "$BANNER" | grep -q 'SPI_FAST_FLASH_BOOT' && {
    echo "✓ 평소 부팅 — 앱이 돌고 있다"
    echo "$BANNER" | grep -i 'boot:' | head -1
}

# 어느 쪽이든 진짜 궁금한 건 이것: 지금 구울 수 있나?
echo "── 구울 수 있는지 확인"
if esptool.py --chip esp32s3 -p "$PORT" --before default_reset --after hard_reset chip_id 2>&1 | grep -q 'Chip is ESP32-S3'; then
    echo "✓ 구울 수 있다. BOOT 버튼 안 눌러도 된다"
else
    echo "✗ 응답 없음 → BOOT 누른 채로 케이블 뽑았다 꽂고 다시 실행"
fi
