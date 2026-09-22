#!/usr/bin/env python3
"""Does the alarm keep counting with the display off? Turn it off and count.

🚨 **This one got us once (2026-09-22).** The header of `app_alarm.c` says the
   alarm has to ring with the app closed and the display off, and `idle_cb`
   takes care to run it above its early returns — and then a later line added
   to save power, `lv_timer_pause(s_idle)`, stopped the very timer that does
   the counting. **The alarm never rang.** Both places read correctly on their
   own, so no amount of reading the code shows it. It has to be measured.

   Also checked: with no alarm set, nothing counts while off (power).

  python3 tools/sim-alarm-tick-check.py
"""
import os, struct, subprocess, sys

D   = os.path.dirname(os.path.abspath(__file__)) + "/.."
SIM = os.path.join(D, "sim", "badge_sim.exe")
if not os.path.exists(SIM):
    SIM = os.path.join(D, "sim", "badge_sim")
KV  = "/tmp/badge_kv_alarm.bin"          # the simulator's NVS is a file (port_sim.c)


def run(armed):
    """Four seconds lit, then dark: how many counts after the display goes off."""
    with open(KV, "wb") as f:
        f.write(struct.pack("BBB", 7, 0, 1 if armed else 0))   # saved_t {h,m,on}
    # 🚨 `P` only accepts under 5000 and **silently drops the rest**
    #    (main_sim.c). Asking for 8000 gives "0 counts" whether the bug is
    #    there or not — which is exactly how this check first lied to us.
    p = subprocess.run([SIM, "--serve"], input=b"P 4000\nW\nP 4000\nP 4000\nQ\n",
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       cwd=D, timeout=90)
    log = p.stderr.decode("utf-8", "replace")
    return log.count("tick alarm veil=0"), log.count("tick alarm veil=1")


bad = []
on, off = run(armed=True)
if on < 2:
    bad.append("nothing counted with the display on (%d) — is the probe still there?" % on)
if off < 6:
    bad.append("alarm set, display off: only %d counts in 8 s — it will not ring" % off)

_, off2 = run(armed=False)
if off2 > 1:
    bad.append("no alarm set, yet %d wake-ups while dark — that is power for nothing" % off2)

try:
    os.remove(KV)
except OSError:
    pass

if bad:
    for b in bad:
        print("  x " + b)
    sys.exit(1)
print("  ok  set: keeps counting while dark (%d) · unset: stays quiet (%d)" % (off, off2))
