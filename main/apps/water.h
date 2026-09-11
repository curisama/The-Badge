/* 물 — Games 안의 한 판. 판을 세우고 도는 타이머를 돌려준다. */
#pragma once
#include "lvgl.h"
lv_timer_t *water_start(lv_obj_t *root);
void        water_stop(void);
void        water_sim_tilt(int x, int y);        /* 시뮬용 가짜 기울기 */
void        water_debug(float *deg, float *boat, float *volume); /* 검증용 */
