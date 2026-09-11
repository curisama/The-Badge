#!/usr/bin/env python3
"""Pulls from the map file how much internal RAM our code holds permanently (.bss).

🚨 A large array left static holds internal RAM even with its app closed.
   Internal RAM is only 116 KB all told and runs out quickly (the water
   particles were 27 KB). Large things have to be allocated in PSRAM and freed.
"""
import re
m = open('build/badge_fw.map', encoding='utf-8', errors='replace').read()
tot = 0
for mm in re.finditer(r'^ \.bss\.\S*\n?\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+\S*libmain\.a', m, re.M):
    tot += int(mm.group(1), 16)
print(tot)
