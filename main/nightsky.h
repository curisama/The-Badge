/* The home wallpaper: a night sky, drawn rather than stored.
 *
 * 🚨 It is code and not a photograph for two reasons. A 466x466 picture is
 * 424 KB of flash, and a photograph someone else took is a licence you have to
 * be able to name — this repository ships no third-party artwork, and the
 * planet maps were taken out for exactly that reason.
 *
 * 🚨 One object, drawn in LV_EVENT_DRAW_MAIN. Not one object per star: past
 * about seventy children lv_timer_handler() stops coming back and the screen
 * freezes with the CPU pinned. That was learnt the hard way on the clock face.
 *
 * An AMOLED draws no current for a black pixel, so a sky that is mostly black
 * with a few hundred lit points is close to free. */
#pragma once
#include "lvgl.h"

/* Builds the wallpaper as a child of parent, sized to the screen. It is not
 * clickable — left clickable it becomes the pressed object for every swipe on
 * the bare background and the home pages stop turning. */
lv_obj_t *nightsky_create(lv_obj_t *parent);
