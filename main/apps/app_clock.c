/* Clock, timer, stopwatch and alarm — all "things that show time", so they
 * live in one app and you swipe between them.
 *
 * The face is the lit variant of the LCD panel. The lock screen draws the
 * same thing on black; see lcdface.h. */
#include "app.h"
#include "port.h"
#include "assets/assets.h"
#include "lcdface.h"
#include <time.h>

#define CX  233
#define CY  233

static lcdface_t  *s_face;
static lv_timer_t *s_timer;

static void tick(lv_timer_t *t)
{
    (void)t;
    lcdface_update(s_face);
}

static void face_build(lv_obj_t *root)
{
    s_face = lcdface_create(root, false);
    s_timer = lv_timer_create(tick, 1000, NULL);
    tick(NULL);
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_CLOCK);
    lv_obj_t *tv = lv_tileview_create(root);
    lv_obj_set_size(tv, 466, 466);
    lv_obj_center(tv);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    /* Clock, timer, stopwatch and alarm are all "things for watching time", so
     * they are one app. Swiped vertically — the middle ones open both ways. */
    face_build     (lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM));
    timer_build    (lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM));
    stopwatch_build(lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP | LV_DIR_BOTTOM));
    alarm_build    (lv_tileview_add_tile(tv, 0, 3, LV_DIR_TOP));
}

static void leave(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    lcdface_destroy(s_face);   /* the objects go with the screen; this is the handle */
    s_face = NULL;
    timer_free();
    stopwatch_free();
    alarm_free();
}

static lv_color_t tint(void) { return lv_color_hex(0xE8E2D2); }

const badge_app_t app_clock = {
    .name = "Clock", .art = &app_icon_clock, .icon = LV_SYMBOL_SETTINGS, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
