#!/usr/bin/env python3
"""우리 코드가 내부 RAM 에 상시로 잡아두는 양(.bss)을 맵에서 뽑는다.

🚨 큰 배열을 static 으로 두면 앱을 안 켜도 내부 RAM 을 물고 있다.
   내부 RAM 은 통틀어 116KB 뿐이라 금방 모자란다(물 입자가 27KB 였다).
   큰 것은 PSRAM 에서 잡고 놓아야 한다.
"""
import re
m = open('build/badge_fw.map', encoding='utf-8', errors='replace').read()
tot = 0
for mm in re.finditer(r'^ \.bss\.\S*\n?\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+\S*libmain\.a', m, re.M):
    tot += int(mm.group(1), 16)
print(tot)
