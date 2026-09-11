#!/usr/bin/env python3
"""Rebuilds the source list in main/CMakeLists.txt from the files that are there.

Forgetting to update the list after baking a new asset breaks the link with
`undefined reference`. mkassets.py calls this as it finishes."""
import glob, io, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")

def rel(pattern):
    # 🚨 On Windows relpath produces backslashes. In CMake a backslash is an
    #    escape and the whole list falls apart — 76 lines were made that way on
    #    09-11. Always written out with forward slashes.
    return sorted(os.path.relpath(p, ROOT).replace(os.sep, "/")
                  for p in glob.glob(os.path.join(ROOT, pattern)))

# Keeping the list by hand means missing a file every time one is added. Everything is swept.

#    This file rewrites CMakeLists every time an icon is made, so a folder
#    missing from this list is quietly dropped. Every folder holding sources has
#    to be named.
srcs = (rel("*.c") + rel("apps/*.c") + rel("ble/*.c")
        + rel("assets/*.c"))

lines = ["idf_component_register(", "    SRCS"]
lines += ["        %s" % s for s in srcs]
# 🚨 "usb" is the directory holding only tusb_config.h. Our own file that reads
# the tinyusb headers (usb_msc.c) has to see it too. On the tinyusb component's
# side the top-level CMakeLists.txt pushes it in separately — that side knows
# nothing about our paths.
lines += ['    INCLUDE_DIRS "." "apps" "ble" "usb")']

# 🚨 The newline and the encoding are pinned. Writing with the Windows defaults
#    mixes in CRLF, and a one-line change then spreads the git diff across the
#    whole file.
io.open(os.path.join(ROOT, "CMakeLists.txt"), "w",
        encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("CMakeLists updated - %d sources" % len(srcs))
