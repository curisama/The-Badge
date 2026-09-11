#!/bin/bash
# PC 시뮬레이터 빌드. 실제 일은 build.py 가 한다.
#
# 🚨 예전엔 여기서 gcc 를 직접 불렀다. 윈도우에서 MSYS 의 fork 흉내가 보안 SW
#    와 부딪혀 죽는 바람에 파이썬으로 옮겼다(0909). 부르던 자리를 안 바꾸려고
#    이 껍데기를 남긴다.
set -e
cd "$(dirname "$0")"
exec "${PYTHON:-python3}" build.py "$@"
