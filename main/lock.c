/* Lock screen — the clock you see first when the display wakes.
 *
 * It draws the always-on variant of the LCD face: black ground, dim
 * segments. On an AMOLED that is the cheapest thing to show, because a lit
 * pixel is the only pixel that costs anything. Waking straight into the
 * bright home wallpaper measures worse, and a light dial would be worse
 * still — which is why the Clock app's panel version does not belong here.
 */
#include "app.h"
#include "port.h"
#include "lcdface.h"

static lv_obj_t   *s_scr;
static lcdface_t  *s_face;
static lv_timer_t *s_tick;

static void tick(lv_timer_t *t)
{
    (void)t;
    lcdface_update(s_face);
}

lv_obj_t *lock_screen(void)
{
    if (s_scr) return s_scr;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_face = lcdface_create(s_scr, true);

    /* Pull the handle up to unlock — back to whichever app was open, or home. */
    launcher_handle_add(s_scr, launcher_unlock);
    return s_scr;
}

void lock_start(void)
{
    if (!s_tick) s_tick = lv_timer_create(tick, 1000, NULL);
    tick(NULL);
}

void lock_stop(void)
{
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
}
