/* 부팅 화면. 켜자마자 아무것도 안 뜨면 켜진 건지 죽은 건지 알 수가 없다.
 * 1.2초 동안 이름과 링 하나를 보여주고 홈으로 넘긴다. */
#include "app.h"

#define SPLASH_MS   1200

static lv_obj_t *s_arc;

static void arc_cb(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, v);
}

static void done_cb(lv_timer_t *t)
{
    (void)t;
    launcher_start();          /* 홈이 페이드로 올라오면서 스플래시를 지운다 */
}

void splash_show(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_screen_load(scr);

    lv_obj_t *name = lv_label_create(scr);
    lv_label_set_text(name, "badge");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xF0F3F6), 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -6);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 300, 300);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1A1A1E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x7FB0FF), LV_PART_INDICATOR);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arc);
    lv_anim_set_exec_cb(&a, arc_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, SPLASH_MS - 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_timer_t *t = lv_timer_create(done_cb, SPLASH_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}
