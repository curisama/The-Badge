#!/usr/bin/env python3
"""Builds the PC simulator. Produces screens as PNGs with no hardware.

    python3 sim/build.py                normal (LVGL comes from the cache)
    SIM_LVGL_REBUILD=1 python3 ...      rebuild LVGL after changing it
    SIM_JOBS=2 python3 ...              change the number of workers

🚨 sim/build.sh used to do this. It does not work on Windows — MSYS's (Git
   Bash's) fork emulation fails when its addresses collide with a DLL injected
   by security software ("dofork: child died unexpectedly, errno 11"). On top
   of that, every process launch is scanned and painfully slow. Python calls
   CreateProcess directly with no fork and meets neither problem. It runs the
   same way on Linux (09-09).
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.chdir(HERE)

# 🚨 LVGL is looked for in two places.
#  1) managed_components — what idf.py fetched (a machine that also builds firmware)
#  2) sim/lvgl — what we fetched for the simulator alone
# --sim-only used to clone by hand into managed_components, and then building
# firmware on the same machine later was refused by idf.py ("no component_hash").
# The simulator's copy stays in its own place so the two do not tread on each
# other (09-09).
def _find_lvgl():
    for c in (Path("../managed_components/lvgl__lvgl"), Path("lvgl")):
        if (c / "lv_version.h").exists():
            return c
    return Path("../managed_components/lvgl__lvgl")   # missing: point at the old place so it errors
LVGL = _find_lvgl()
CC = os.environ.get("CC", "gcc")
AR = os.environ.get("AR", "ar")
JOBS = int(os.environ.get("SIM_JOBS") or os.cpu_count() or 4)

CFLAGS = ["-O1", "-w", "-DBADGE_SIM", "-DLV_CONF_INCLUDE_SIMPLE"]
if os.environ.get("SIM_TIGHT"):
    CFLAGS.append("-DBADGE_SIM_TIGHT")
if os.name == "nt":
    # 🚨 Windows has no POSIX functions like localtime_r. The shim goes in.
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
    """Turns a source path into an object name. The whole path is used so files in different folders cannot collide."""
    flat = src.as_posix().replace("/", "_").replace(".", "_")
    return Path(into) / (flat + ".o")


def is_fresh(src: Path, obj: Path) -> bool:
    """Skips rebuilding when the object is newer than the source and newer than
    the headers it pulls in.

    🚨 Looking only at the source time reuses a stale object after a header
       changes. The list gcc leaves with -MMD is checked too.

    🚨 That list still cannot see a header that did not exist when the object
       was built. orb.c picks up its planet maps with __has_include, so baking
       a map for the first time changes what compiles while every timestamp
       says nothing moved — the simulator went on drawing code planets over
       real ones. Touch the source after adding a file it can conditionally
       include."""
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
    # gcc breaks long lines with "backslash + newline". They are joined back up.
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
    # 🚨 Built under a temporary name and then moved. Dying midway leaves a
    #    half-written object, and the next build takes it for a good one.
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
    print(f"{label}: building {len(todo)} (workers {JOBS})", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=JOBS) as pool:
        for err in pool.map(lambda s: compile_one(s, into, skip_fresh), todo):
            done += 1
            if err:
                die(f"\n✗ compile failed\n{err}")
            if done % 50 == 0 or done == len(todo):
                print(f"  {done}/{len(todo)}", flush=True)


def main():
    if not LVGL.is_dir():
        die(f"✗ LVGL is missing: {LVGL}\n  run idf.py reconfigure or tools/setup.sh --sim-only first")
    if shutil.which(CC) is None:
        die(f"✗ {CC} not found. On Windows, install mingw-w64 and put it on PATH")

    # ── 1. LVGL — built once and cached ────────────────────────────
    # 🚨 LVGL alone is 463 .c files. Rebuilding every time takes 36 minutes on
    #    Windows — nobody can work that way with one line of badge code to fix.
    #    It is somebody else's code that we never touch, so it is built once
    #    into liblvgl.a.
    #
    # 🚨 What counts as "the same LVGL" matters. Using the git SHA lost it every
    #    time `idf.py` replaced managed_components with its own copy (a
    #    downloaded package has no .git), and all 463 files were rebuilt from
    #    scratch. The version number and the file count are used instead — the
    #    same 9.5.0 is the same code.
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
    stamp = f"lvgl {ver} · {len(lvgl_srcs)} files | " + " ".join(CFLAGS)
    stamp_f = Path("liblvgl.stamp")
    lib = Path("liblvgl.a")
    stale = (os.environ.get("SIM_LVGL_REBUILD") or not lib.exists()
             or not stamp_f.exists() or stamp_f.read_text(encoding="utf-8") != stamp)
    if stale:
        lib.unlink(missing_ok=True)
        stamp_f.unlink(missing_ok=True)
        compile_all(lvgl_srcs, "lvgl_obj", True, "LVGL")
        objs = [str(obj_for(s, "lvgl_obj")) for s in lvgl_srcs]
        # 🚨 Handing 463 objects over in one line exceeds Windows' command-line
        #    limit (32767 characters), so they are appended in batches. And the
        #    archive is built under a temporary name and moved at the end —
        #    dying mid-append leaves an archive holding 400 of 463 that looks
        #    plausible by size, gets taken for good, and sends the link wandering
        #    (09-09).
        part = Path(str(lib) + ".part")
        part.unlink(missing_ok=True)
        for i in range(0, len(objs), 80):
            r = subprocess.run([AR, "rcs", str(part), *objs[i:i + 80]],
                               capture_output=True, text=True, errors="replace")
            if r.returncode != 0:
                part.unlink(missing_ok=True)
                die(f"✗ ar failed\n{r.stdout}{r.stderr}")
        os.replace(part, lib)
        stamp_f.write_text(stamp, encoding="utf-8")
        print(f"  → sim/liblvgl.a ({len(objs)} objects)")

    # ── 2. badge code — rebuilt every time ─────────────────────────
    # 🚨 It used to wipe everything each time. Builds on this PC are often cut
    #    short (the memory watchdog) and every one of those started over.
    #    Headers are taken into account, so building on top is safe.
    compile_all(BADGE_SRCS, "badge_obj", True, "badge")

    # ── 3. linking ─────────────────────────────────────────────────
    # 🚨 --start-group is needed. A static archive is scanned only once, so an
    #    LVGL object pulled in later cannot find one that was skipped earlier.
    #
    # 🚨 But that is a GNU ld option and **Apple's linker (ld64) does not know
    #    it** — passing it on a Mac kills the link with "unknown options". ld64
    #    does not scan only once to begin with, so it needs no group markers.
    #    The platform decides.
    out = "badge_sim"
    if sys.platform == "darwin":
        libargs = [str(lib)]
    else:
        libargs = ["-Wl,--start-group", str(lib), "-Wl,--end-group"]
    cmd = [CC, *[str(obj_for(s, "badge_obj")) for s in BADGE_SRCS],
           *libargs, "-lm", "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        die(f"✗ link failed\n{r.stdout}{r.stderr}")
    made = Path(out + ".exe") if Path(out + ".exe").exists() else Path(out)
    print(f"build finished → sim/{made.name} ({made.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
