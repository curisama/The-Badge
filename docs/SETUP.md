# 다른 컴퓨터에서 이어서 개발하기

집·회사 양쪽에서 같은 상태로 만지려고 정리해둔다.
저장소만 받으면 되게 해뒀지만, **깃에 안 들어가는 게 넷** 있다. 그것만 챙기면 된다.

## 깃에 없는 것 (일부러 뺐다)

| | 왜 | 어떻게 |
|---|---|---|
| `main/secrets.h` | WiFi 비번이 들었다 | 아래 양식대로 직접 만든다 |
| `managed_components/` | 남의 코드다 (LVGL 등 22개) | `idf.py reconfigure` 가 알아서 받는다 |
| `sdkconfig` | 기계마다 다시 생긴다 | `sdkconfig.defaults` 에서 자동 생성 |
| `build/` | 산출물 | 다시 빌드하면 된다 |

🚨 `sdkconfig` 를 깃에 넣지 마라. 사람이 손댄 설정은 전부 `sdkconfig.defaults`
   에 있어야 한다 — 거기 없으면 다른 기계에서 조용히 다른 펌웨어가 나온다
   (PSRAM 속도, 파티션 표, 라이트슬립 같은 것들이다).

## 시뮬레이터만 쓸 때 (배지 없이)

오늘 만든 것 대부분이 이걸로 검증됐다 — 물 입자, 천체 회전, 폰트, 게임 로직.

```bash
git clone https://github.com/curisama/amoled-badge && cd amoled-badge
./tools/setup.sh --sim-only        # LVGL 만 받아서 시뮬 빌드
python3 sim/server.py              # 브라우저로 localhost:8791
```

## 펌웨어까지 구울 때

ESP-IDF v5.5 가 필요하다 (몇 GB 받는다).

```bash
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.5.5 ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh

cd amoled-badge
cp main/secrets.example.h main/secrets.h   # 비번 채운다
idf.py build
idf.py -p /dev/ttyACM0 flash monitor       # 윈도는 COMx
```

## 양쪽에서 만질 때 지킬 것

- **브랜치**: 실사용은 `panel-power`, 실험은 따로 판다. `master` 는 아직 안 건드린다.
- **시작 전에 `git pull`.** 한쪽에서만 고치고 다른 쪽에서 또 고치면 합치기 번거롭다.
- **끝나면 푸시.** 반쯤 된 것도 브랜치 파서 올려두는 게 낫다.

## 손대기 전후로 돌릴 검사

```bash
./tools/regress.sh              # 지난 오류 16종이 되살아났나 (소스만 본다)
python3 tools/sim-exit-check.py # 판마다 홈으로 나오다 죽나
python3 tools/sim-stress.py     # 시계 흔들고 난폭 조작
./tools/check-glyphs.sh         # 폰트에 없는 글자가 화면에 나가나
```

배지가 있으면 이것도:

```bash
./tools/badge-diag.sh           # 재부팅 사유·배터리 일지 (아무것도 안 지운다)
./tools/check-run.sh <로그>     # 실행 로그를 사람 기준으로 판정
```
