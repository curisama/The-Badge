#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* BSP 대신 우리가 올리는 디스플레이. 패널 핸들을 쥐고 있어야
 * 화면을 진짜로 끌 수 있다(BSP 는 핸들을 안 내준다). */
bool        badge_display_start(void);
lv_indev_t *badge_display_indev(void);
void        badge_display_rotate180(void);
void        badge_display_brightness(int percent);   /* 0 = 화소 끔 */
int         badge_display_brightness_get(void);      /* 0 은 기억 안 한다 */
void        badge_display_on(bool on);
/* 180도 뒤집었을 때의 x 여백. 안 맞으면 가장자리에 색 띠가 남는다.
 * 문서가 없어 컨트롤러 열 수를 모르므로 설정에서 고른다. */
void        badge_display_set_xgap(int gap);
int         badge_display_get_xgap(void);               /* 패널 자체를 켜고 끈다 (0x28/0x29) */
