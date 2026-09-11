#!/usr/bin/env python3
"""main/CMakeLists.txt 의 소스 목록을 실제 파일에서 다시 만든다.

에셋을 새로 구운 뒤 목록 갱신을 잊으면 `undefined reference` 로 링크가 깨진다.
mkassets.py 가 끝나면서 이걸 부른다."""
import glob, io, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")

def rel(pattern):
    # 🚨 윈도우에선 relpath 가 역슬래시를 낸다. CMake 에선 역슬래시가
    #    이스케이프라 목록이 통째로 깨진다 — 0911 에 76줄을 그렇게 만들었다.
    #    항상 앞슬래시로 바꿔서 내보낸다.
    return sorted(os.path.relpath(p, ROOT).replace(os.sep, "/")
                  for p in glob.glob(os.path.join(ROOT, pattern)))

# 목록을 손으로 들고 있으면 파일을 새로 만들 때마다 빠뜨린다. 전부 훑는다.

#    아이콘을 새로 만들 때마다 이 파일이 CMakeLists 를 다시 쓰므로,
#    여기 목록에 없는 폴더는 조용히 지워진다. 소스가 있는 폴더는 다 적어야 한다.
srcs = (rel("*.c") + rel("apps/*.c") + rel("ble/*.c")
        + rel("assets/*.c"))

lines = ["idf_component_register(", "    SRCS"]
lines += ["        %s" % s for s in srcs]
# 🚨 "usb" 는 tusb_config.h 한 장만 든 칸이다. tinyusb 헤더를 읽는 우리 쪽
# 파일(usb_msc.c)도 그게 보여야 한다. tinyusb 컴포넌트 쪽에는 최상위
# CMakeLists.txt 가 따로 끼워 넣는다 — 그쪽은 우리 경로를 모른다.
lines += ['    INCLUDE_DIRS "." "apps" "ble" "usb")']

# 🚨 개행문자와 인코딩을 박아둔다. 윈도우 기본값으로 쓰면 CRLF 가 섞여
#    한 줄만 바뀌어도 깃 차이가 파일 전체로 번진다.
io.open(os.path.join(ROOT, "CMakeLists.txt"), "w",
        encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("CMakeLists updated - %d sources" % len(srcs))
