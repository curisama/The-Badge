#!/usr/bin/env python3
"""PC 시뮬레이터 빌드. 하드웨어 없이 화면을 PNG 로 뽑는다.

    python3 sim/build.py                평소 (LVGL 은 쟁여둔 것을 쓴다)
    SIM_LVGL_REBUILD=1 python3 ...      LVGL 을 갈았을 때 다시 짓게 한다
    SIM_JOBS=2 python3 ...              일꾼 수를 바꾼다

🚨 예전엔 이 일을 sim/build.sh 가 했다. 윈도우에선 그게 안 된다 — MSYS(깃배시)
   의 fork 흉내가 보안 SW 가 밀어 넣은 DLL 과 주소가 겹쳐 실패한다
   ("dofork: child died unexpectedly, errno 11"). 게다가 프로세스 하나 띄우는
   데 검사가 붙어 몹시 느리다. 파이썬은 fork 없이 CreateProcess 를 그냥
   부르므로 둘 다 안 겪는다. 리눅스에서도 똑같이 돈다(0909).
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)

# 🚨 LVGL 을 두 군데서 찾는다.
#  1) managed_components — idf.py 가 받아 둔 것 (펌웨어도 굽는 기계)
#  2) sim/lvgl — 시뮬만 쓰려고 우리가 받아 둔 것
# 예전엔 --sim-only 가 managed_components 에 손으로 클론했는데, 나중에 같은
# 기계에서 펌웨어를 구우려 하면 idf.py 가 거부한다("component_hash 없음").
# 시뮬용은 제 자리에 두어 서로 안 밟게 한다(0909).
def _find_lvgl():
    for c in (Path("../managed_components/lvgl__lvgl"), Path("lvgl")):
        if (c / "lv_version.h").exists():
            return c
    return Path("../managed_components/lvgl__lvgl")   # 없으면 예전 자리를 가리켜 오류를 내게
LVGL = _find_lvgl()
CC = os.environ.get("CC", "gcc")
AR = os.environ.get("AR", "ar")
JOBS = int(os.environ.get("SIM_JOBS") or os.cpu_count() or 4)

CFLAGS = ["-O1", "-w", "-DBADGE_SIM", "-DLV_CONF_INCLUDE_SIMPLE"]
if os.environ.get("SIM_TIGHT"):
    CFLAGS.append("-DBADGE_SIM_TIGHT")
if os.name == "nt":
    # 🚨 윈도우엔 localtime_r 같은 POSIX 함수가 없다. 다리를 끼워 넣는다.
    CFLAGS += ["-include", "win_compat.h"]
INC = ["-I.", "-I../main", "-I../main/apps", f"-I{LVGL}", f"-I{LVGL}/src"]

# Files that only make sense on the board. Everything else in main/ is shared
# with the simulator and picked up automatically.
#
# This used to be a hand-written list of the three shared files, which meant a
# new shared file linked fine on the board and failed here with an undefined
# reference. Naming what to leave out is the shorter list and it does not grow
# every time a file is added.
ESP_ONLY = {
    "main.c",        # app_main, the board's entry point
    "port_esp.c",    # the board half of port.h (port_sim.c stands in)
    "display.c",     # QSPI panel bring-up
    "adpcm.c", "rec_store.c", "rec_upload.c",   # recording, flash-backed
    "usb_export.c", "usb_msc.c",                # USB mass storage
}

BADGE_SRCS = [
    Path("main_sim.c"), Path("port_sim.c"),
    *[p for p in sorted(Path("../main").glob("*.c")) if p.name not in ESP_ONLY],
    *sorted(Path("../main/apps").glob("*.c")),
    *sorted(Path("../main/assets").glob("*.c")),
]


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def obj_for(src: Path, into: str) -> Path:
    """소스 경로를 조각 이름으로. 폴더가 달라도 안 겹치게 경로째 쓴다."""
    flat = src.as_posix().replace("/", "_").replace(".", "_")
    return Path(into) / (flat + ".o")


def is_fresh(src: Path, obj: Path) -> bool:
    """조각이 소스보다 새롭고, 딸려 있는 헤더보다도 새로우면 다시 안 짓는다.

    🚨 소스 시각만 보면 헤더를 고쳤을 때 옛 조각을 그대로 쓴다. gcc 가
       -MMD 로 남긴 목록을 같이 본다."""
    if not obj.exists():
        return False
    t = obj.stat().st_mtime
    if t < src.stat().st_mtime:
        return False
    dep = obj.with_name(obj.name[:-2] + ".d")
    if not dep.exists():
        return False
    try:
        text = dep.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    # gcc 는 긴 줄을 "역슬래시 + 줄바꿈" 으로 끊어 적는다. 이어 붙인다.
    text = text.replace("\\\n", " ")
    _, _, rhs = text.partition(":")
    for w in rhs.split():
        try:
            if os.stat(w).st_mtime > t:
                return False
        except OSError:
            return False
    return True


def compile_one(src: Path, into: str, skip_fresh: bool):
    obj = obj_for(src, into)
    if skip_fresh and is_fresh(src, obj):
        return None
    # 🚨 임시 이름으로 지은 뒤 옮긴다. 중간에 죽으면 반만 써진 조각이 남는데,
    #    다음 빌드가 그걸 멀쩡한 줄 알고 쓴다.
    tmp = obj.parent / (obj.name + ".part")
    dep = obj.with_name(obj.name[:-2] + ".d")
    r = subprocess.run([CC, *CFLAGS, *INC, "-MMD", "-MF", str(dep),
                        "-c", str(src), "-o", str(tmp)],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        tmp.unlink(missing_ok=True)
        return f"{src}\n{r.stdout}{r.stderr}"
    os.replace(tmp, obj)
    return None


def compile_all(srcs, into: str, skip_fresh: bool, label: str):
    Path(into).mkdir(exist_ok=True)
    todo = [s for s in srcs if not (skip_fresh and is_fresh(s, obj_for(s, into)))]
    if not todo:
        return
    print(f"{label}: {len(todo)}개 짓는다 (일꾼 {JOBS})", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=JOBS) as pool:
        for err in pool.map(lambda s: compile_one(s, into, skip_fresh), todo):
            done += 1
            if err:
                die(f"\n✗ 컴파일 실패\n{err}")
            if done % 50 == 0 or done == len(todo):
                print(f"  {done}/{len(todo)}", flush=True)


def main():
    if not LVGL.is_dir():
        die(f"✗ LVGL 이 없다: {LVGL}\n  idf.py reconfigure 또는 tools/setup.sh --sim-only 를 먼저")
    if shutil.which(CC) is None:
        die(f"✗ {CC} 를 못 찾았다. 윈도우면 mingw-w64 를 깔고 PATH 에 넣어라")

    # ── 1. LVGL — 한 번만 짓고 쟁여둔다 ────────────────────────────
    # 🚨 LVGL 만 .c 가 463개다. 매번 다시 지으면 윈도우에서 36분 걸린다 —
    #    배지 코드 한 줄 고칠 때마다 그러면 일을 못 한다. 우리가 안 건드리는
    #    남의 코드니 한 번 지어 liblvgl.a 에 둔다.
    #
    # 🚨 무엇을 보고 "같은 LVGL" 이라 할지가 중요하다. 깃 SHA 를 봤더니
    #    `idf.py` 가 managed_components 를 제 것으로 갈아치울 때(내려받은
    #    꾸러미엔 .git 이 없다) 매번 SHA 를 잃고 463개를 처음부터 다시 지었다.
    #    판 번호와 파일 수를 본다 — 같은 9.5.0 이면 같은 코드다.
    lvgl_srcs = sorted(LVGL.glob("src/**/*.c"))
    ver = "?"
    vh = LVGL / "lv_version.h"
    if vh.exists():
        nums = {}
        for line in vh.read_text(encoding="utf-8", errors="replace").splitlines():
            for key in ("MAJOR", "MINOR", "PATCH"):
                tag = f"#define LVGL_VERSION_{key}"
                if line.startswith(tag):
                    nums[key] = line[len(tag):].strip()
        if len(nums) == 3:
            ver = f"{nums['MAJOR']}.{nums['MINOR']}.{nums['PATCH']}"
    stamp = f"lvgl {ver} · {len(lvgl_srcs)}개 | " + " ".join(CFLAGS)
    stamp_f = Path("liblvgl.stamp")
    lib = Path("liblvgl.a")
    stale = (os.environ.get("SIM_LVGL_REBUILD") or not lib.exists()
             or not stamp_f.exists() or stamp_f.read_text(encoding="utf-8") != stamp)
    if stale:
        lib.unlink(missing_ok=True)
        stamp_f.unlink(missing_ok=True)
        compile_all(lvgl_srcs, "lvgl_obj", True, "LVGL")
        objs = [str(obj_for(s, "lvgl_obj")) for s in lvgl_srcs]
        # 🚨 조각 463개를 한 줄에 넘기면 윈도우 명령줄 한도(32767자)를 넘는다.
        #    나눠 덧붙인다. 그리고 임시 이름에 지은 뒤 마지막에 옮긴다 —
        #    덧붙이는 중에 죽으면 400/463 만 든 묶음이 남는데, 크기도 그럴싸해서
        #    멀쩡한 줄 알고 쓰다가 링크에서 엉뚱한 데를 헤맨다(0909).
        part = Path(str(lib) + ".part")
        part.unlink(missing_ok=True)
        for i in range(0, len(objs), 80):
            r = subprocess.run([AR, "rcs", str(part), *objs[i:i + 80]],
                               capture_output=True, text=True, errors="replace")
            if r.returncode != 0:
                part.unlink(missing_ok=True)
                die(f"✗ ar 실패\n{r.stdout}{r.stderr}")
        os.replace(part, lib)
        stamp_f.write_text(stamp, encoding="utf-8")
        print(f"  → sim/liblvgl.a ({len(objs)}개)")

    # ── 2. 배지 코드 — 매번 다시 짓는다 ────────────────────────────
    # 🚨 예전엔 매번 통째로 지웠다. 이 PC 에선 빌드가 자주 끊겨(메모리 감시기)
    #    그때마다 처음부터가 됐다. 헤더까지 보고 판단하니 이어 지어도 안전하다.
    compile_all(BADGE_SRCS, "badge_obj", True, "배지")

    # ── 3. 묶기 ────────────────────────────────────────────────────
    # 🚨 --start-group 이 필요하다. 정적 묶음은 한 번만 훑기 때문에, 나중에
    #    꺼낸 LVGL 조각이 앞서 건너뛴 조각을 찾으면 못 찾는다.
    #
    # 🚨 그런데 그건 GNU ld 옵션이고 **애플 링커(ld64)는 모른다** — 맥에서
    #    그대로 주면 "unknown options" 로 링크가 죽는다. ld64 는 애초에 한
    #    번만 훑지 않아서 묶음 표시가 필요 없다. 기계를 보고 가른다.
    out = "badge_sim"
    if sys.platform == "darwin":
        libargs = [str(lib)]
    else:
        libargs = ["-Wl,--start-group", str(lib), "-Wl,--end-group"]
    cmd = [CC, *[str(obj_for(s, "badge_obj")) for s in BADGE_SRCS],
           *libargs, "-lm", "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        die(f"✗ 링크 실패\n{r.stdout}{r.stderr}")
    made = Path(out + ".exe") if Path(out + ".exe").exists() else Path(out)
    print(f"빌드 완료 → sim/{made.name} ({made.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
