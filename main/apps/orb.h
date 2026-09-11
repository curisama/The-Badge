/* 달·지구 — 손가락으로 돌리는 구. Games 안의 한 판. */
#pragma once
#include "lvgl.h"

typedef enum { ORB_MOON, ORB_EARTH, ORB_SUN, ORB_JUPITER, ORB_N } orb_kind_t;
extern const char *const ORB_NAME[ORB_N];

lv_timer_t *orb_start(lv_obj_t *root, orb_kind_t kind);
void        orb_stop(void);
/* 검증용 — 지금 경도(0~1)와 프레임 하나 그리는 데 걸린 마이크로초 */
void        orb_debug(float *lon, uint32_t *render_us);
void        orb_set_tilt_deg(float deg);   /* 검증용 */
void        orb_set_lon(float lon);        /* 검증용 */
void        orb_set_lean_deg(float deg);   /* 검증용 */
