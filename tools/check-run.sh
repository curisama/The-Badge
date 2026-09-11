#!/usr/bin/env bash
# 실행 로그 한 판을 사람이 읽는 기준으로 판정한다.
#
# 🚨 빌드 출력과 실행 로그가 같은 파일에 섞인다. 예전에 'panic' 으로 grep
#    했다가 컴파일된 파일 이름(panic_handler.c)까지 세어 "뻗음 6회" 로 읽었다.
#    그래서 여기선 실행 줄(로그 레벨로 시작하거나 부팅 표시가 있는 줄)만 본다.
set -u
L=${1:?사용법: check-run.sh <로그파일>}
RUN=$(mktemp)
sed 's/\x1b\[[0-9;]*m//g' "$L" | grep -E '^[IWED] \(|^rst:0x|Guru Meditation|Backtrace' > "$RUN"

draw=$(grep -cE 'priv TX buffer|Draw bitmap failed|transmit \(queue\) color' "$RUN")
dead=$(grep -cE 'task_wdt: Task watchdog|Guru Meditation|abort\(\) was called' "$RUN")
boot=$(grep -cE '^rst:0x' "$RUN"); boot=$((boot > 0 ? boot - 1 : 0))
# 🚨 원인을 아는 무해한 오류는 따로 센다 — 지우지는 않는다.
#    i2s_channel_disable "not been enabled yet":
#      마이크 코덱 핸들이 송·수신 한 몸이라 닫을 때 안 쓴 송신 채널까지
#      끄려 든다. 녹음 자체는 정상으로 기록된다(0908 검증에서 15건 다 남았다).
#      벤더 부품 안쪽이라 손 안 댄다. 펌웨어의 건강검사도 같은 기준이다.
KNOWN='has not been enabled yet'
known=$(grep -E '^E \(' "$RUN" | grep -cE "$KNOWN")
err=$(grep -E '^E \(' "$RUN" | grep -vcE "$KNOWN")

echo "그리기 실패 ${draw}회   ← 0 이어야 통과"
echo "뻗음        ${dead}회"
echo "예상밖 재부팅 ${boot}회"
echo "E 오류      ${err}건   (무해로 설명된 것 ${known}건은 따로)"
if [ "$err" -gt 0 ]; then echo "── 설명 안 되는 E 오류"; grep -E '^E \(' "$RUN" | grep -vE "$KNOWN" | sort -u | head; fi
grep -E '검증 끝' "$RUN" | sed 's/^.*badge: //'
rm -f "$RUN"
[ "$draw" -eq 0 ] && [ "$dead" -eq 0 ] && [ "$boot" -eq 0 ] && [ "$err" -eq 0 ] \
  && echo "→ 통과" || { echo "→ 통과 아님"; exit 1; }
