# Changelog

What changed, and — more usefully — why it broke in the first place.

The [README](README.md) calls the checks "a list of scars". This file is the
same idea for behaviour: each entry tries to leave you with something you can
use on your own board, not just a version number.

## 2026-09-21

### Fixed — the stopwatch started when you swiped away from it

**What you saw.** You swipe up to leave the stopwatch, and the moment your
finger lifts the clock is running. Swipe back in to stop it, and now it has a
stray lap. Annoying in the way that makes you stop trusting the screen.

**Why.** The stopwatch takes the whole screen as its button — there is no small
"start" target to hit, which is the right call on a 466 px round display you
are looking at from a metre away. It watched `PRESSED` to start a long-press
timer, and `RELEASED` to toggle the run state:

```c
if (code == LV_EVENT_RELEASED) {
    if (!s_long_done) set_run(!s_run);
}
```

That is correct for a tap and wrong for everything else. A swipe is also a
press followed by a release — the app never asked **where** the finger went in
between, so a gesture meant for the launcher landed on the stopwatch on its way
out.

**The fix.** Remember where the press started; on release, ignore it if the
finger travelled more than 60 px. Long-press (lap / reset) is untouched.

```c
int dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
if (dx * dx + dy * dy >= 60 * 60) return;   /* that was a swipe */
```

**If you are building something similar.** Any full-screen tap target on a
device whose main navigation is a swipe has this bug until you add the distance
check. It will not show up while you are testing the feature itself — you only
find it once the screen has a neighbour to swipe to. Ours hid for months.

The same guard already lives in the launcher and in the badge's key app; this
just brings the stopwatch in line with them.
