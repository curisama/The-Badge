/* Moon and Earth — a sphere turned with a finger. One of the boards in Games. */
#pragma once
#include "lvgl.h"

typedef enum { ORB_MOON, ORB_EARTH, ORB_SUN, ORB_JUPITER, ORB_N } orb_kind_t;
extern const char *const ORB_NAME[ORB_N];

lv_timer_t *orb_start(lv_obj_t *root, orb_kind_t kind);
void        orb_stop(void);
/* For checking — the current longitude (0..1) and the microseconds one frame took */
void        orb_debug(float *lon, uint32_t *render_us);
void        orb_set_tilt_deg(float deg);   /* for checking */
void        orb_set_lon(float lon);        /* for checking */
void        orb_set_lean_deg(float deg);   /* for checking */
