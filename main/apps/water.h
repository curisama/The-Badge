/* Water — one of the boards in Games. Sets the board up and hands back the timer that runs it. */
#pragma once
#include "lvgl.h"
lv_timer_t *water_start(lv_obj_t *root);
void        water_stop(void);
void        water_sim_tilt(int x, int y);        /* fake tilt, for the simulator */
void        water_debug(float *deg, float *boat, float *volume); /* for checking */
