#!/usr/bin/env bash
# 새 컴퓨터에서 이 저장소를 굴릴 수 있게 만든다.
#
#   ./tools/setup.sh --sim-only   시뮬레이터만 (배지 없이, ESP-IDF 불필요)
#   ./tools/setup.sh              펌웨어까지 (ESP-IDF 가 이미 깔려 있어야 한다)
set -eu
cd "$(dirname "$0")/.."
SIM_ONLY=0
[ "${1:-}" = "--sim-only" ] && SIM_ONLY=1

echo "════ 1. 비번 파일"
if [ -f main/secrets.h ]; then
    echo "  이미 있다"
else
    cp main/secrets.example.h main/secrets.h
    echo "  main/secrets.h 를 만들었다 — 열어서 SSID·비번을 채워라"
fi

echo "════ 2. 남의 코드(LVGL 등)"
if [ -d managed_components/lvgl__lvgl ] || [ -d sim/lvgl ]; then
    echo "  이미 있다"
elif [ "$SIM_ONLY" = 1 ]; then
    # 🚨 시뮬은 LVGL 소스만 있으면 된다. ESP-IDF 를 몇 GB 받지 않아도
    #    여기까지는 굴릴 수 있다 — 회사 PC 처럼 무거운 설치가 곤란한 데서 쓴다.
    #
    # 🚨 예전엔 managed_components/ 에 손으로 클론했다. 그러면 나중에 같은
    #    기계에서 펌웨어를 구우려 할 때 idf.py 가 거부한다
    #    ("component_hash 또는 CHECKSUMS.json 이 없다"). 그래서 sim/lvgl 에
    #    따로 둔다 — build.py 가 두 군데를 다 본다(0909 회사 PC 에서 걸림).
    V=$(grep -A2 'lvgl/lvgl' main/idf_component.yml | grep version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    echo "  LVGL ${V} 를 받는다 (시뮬 전용, sim/lvgl)"
    git clone --depth 1 -b "v${V}" https://github.com/lvgl/lvgl.git sim/lvgl
else
    command -v idf.py >/dev/null || { echo "  ✗ ESP-IDF 환경이 아니다. . ~/esp/esp-idf/export.sh 먼저"; exit 1; }
    idf.py reconfigure >/dev/null
    echo "  받았다"
fi

echo "════ 3. 시뮬레이터 빌드"
bash sim/build.sh

if [ "$SIM_ONLY" = 0 ]; then
    echo "════ 4. 펌웨어 빌드"
    command -v idf.py >/dev/null && idf.py build || echo "  건너뜀 (ESP-IDF 환경 아님)"
fi

echo
echo "→ 끝났다."
echo "   시뮬:  python3 sim/server.py   그리고 브라우저로 localhost:8791"
# 🚨 배지가 잡히는 이름이 기계마다 다르다. 리눅스는 /dev/ttyACM0, 맥은
#    /dev/cu.usbmodem* (ESP32-S3 는 네이티브 USB CDC 라 usbmodem 으로 뜬다),
#    윈도우는 COMx. 문서에 리눅스 것만 적어두면 다른 데선 헤맨다.
# 🚨 `[ 조건 ] && echo` 를 마지막 줄에 두면 안 된다. 조건이 거짓일 때
# 스크립트가 1 로 끝나고, 이걸 부르는 쪽이 set -e 면 거기서 중단된다 —
# 꾸러미가 제 안내문을 못 찍고 죽었다(0909).
if [ "$SIM_ONLY" = 0 ]; then
    case "$(uname -s)" in
        Darwin) echo "   굽기:  idf.py -p \$(ls /dev/cu.usbmodem* | head -1) flash monitor" ;;
        *)      echo "   굽기:  idf.py -p /dev/ttyACM0 flash monitor" ;;
    esac
fi
exit 0
