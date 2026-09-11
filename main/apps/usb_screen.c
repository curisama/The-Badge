/* "녹음 내보내기" 화면.
 *
 * 🚨 이 화면이 켜져 있는 동안 배지의 USB-C 는 COM 이 아니다. 그래서 **여기가
 * 모드를 바꾸는 유일한 문**이고, 나가는 길이 늘 보여야 한다. 실수로 들어와서
 * 못 나오면 굽지도 못하는 물건이 된다.
 *
 * 🚨 "끝" 은 재부팅이다(usb_msc.c 의 설명). 사람에게도 그렇게 적는다 —
 * 아무 말 없이 꺼졌다 켜지면 고장난 줄 안다. */
#include "app.h"
#include "port.h"
#include "usb_export.h"
#include <stdio.h>

static lv_obj_t   *s_scr, *s_btn, *s_big, *s_sub, *s_note;
static lv_timer_t *s_tick;

static void usb_screen_close(void);

static void paint(void)
{
    if (!s_big) return;
    bool on = usb_msc_active();
    int  n  = usb_export_files();

    if (!on) {
        lv_label_set_text(s_big, LV_SYMBOL_DRIVE);
        if (n > 0) lv_label_set_text_fmt(s_sub, "%d recordings", n);
        else       lv_label_set_text(s_sub, "nothing to send");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(n > 0 ? 0x16324A : 0x14161C), 0);
        lv_obj_set_style_border_color(s_btn, lv_color_hex(n > 0 ? 0x3E7FB8 : 0x24262E), 0);
        lv_label_set_text(s_note, n > 0 ? "tap to become a USB drive" : "record something first");
        return;
    }

    /* 켜진 뒤로는 배지가 드라이브다. 호스트가 실제로 붙었는지까지 보여준다 —
     * "켰는데 안 보인다" 가 케이블 문제인지 배지 문제인지 갈린다. */
    if (usb_msc_ejected()) {
        lv_label_set_text(s_big, LV_SYMBOL_OK);
        lv_label_set_text(s_sub, "ejected");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x1C4034), 0);
        lv_obj_set_style_border_color(s_btn, lv_color_hex(0x5BD48A), 0);
        lv_label_set_text(s_note, "tap to go back to COM (restarts)");
    } else if (usb_msc_mounted()) {
        lv_label_set_text(s_big, LV_SYMBOL_DRIVE);
        lv_label_set_text_fmt(s_sub, "%d files", usb_export_files());
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x1C4034), 0);
        lv_obj_set_style_border_color(s_btn, lv_color_hex(0x5BD48A), 0);
        lv_label_set_text(s_note, "eject on the PC, or tap to finish");
    } else {
        lv_label_set_text(s_big, LV_SYMBOL_REFRESH);
        lv_label_set_text(s_sub, "waiting");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x3A3216), 0);
        lv_obj_set_style_border_color(s_btn, lv_color_hex(0xE0B33A), 0);
        lv_label_set_text(s_note, "plug into a PC");
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    /* 호스트가 안전 제거를 하면 그 자리에서 COM 으로 되돌린다 —
     * 사람이 다시 배지를 만질 필요가 없다. */
    if (usb_msc_active() && usb_msc_ejected()) { paint(); usb_msc_stop(); return; }
    paint();
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    if (usb_msc_active()) { usb_msc_stop(); return; }   /* 끝 — 재부팅한다 */
    if (usb_export_files() <= 0) return;
    if (!usb_msc_start()) lv_label_set_text(s_note, "could not switch to USB");
    paint();
}

static void usb_screen_close(void)
{
    /* 🚨 드라이브로 올라간 뒤엔 못 빠져나간다 — 나가려면 모드를 되돌려야
     * 하고 그건 재부팅이다. 문을 잠그는 대신 그 일을 대신 해준다. */
    if (usb_msc_active()) { usb_msc_stop(); return; }
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    launcher_keep_awake(false);
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_btn = s_big = s_sub = s_note = NULL;
}

void usb_screen_open(void)
{
    if (s_scr) return;
    usb_export_build();              /* 지금 있는 녹음으로 판을 짜 둔다 */

    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(s_scr);
    lv_label_set_text(t, "USB export");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -186);

    s_btn = lv_obj_create(s_scr);
    lv_obj_set_size(s_btn, 260, 260);
    lv_obj_center(s_btn);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_btn, 4, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn, tap_cb, LV_EVENT_CLICKED, NULL);

    s_big = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_big, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_big, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -16);

    s_sub = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0xB6C2D6), 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 34);

    s_note = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_note, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 178);

    ui_back_btn(s_scr, usb_screen_close);
    launcher_handle_add(s_scr, usb_screen_close);
    /* 🚨 내보내는 동안 화면이 꺼지면 안 된다. 꺼진 화면에서는 "끝" 을 누를
     * 길이 없고, 그 상태로 케이블을 뽑으면 윈도우가 짖는다. */
    launcher_keep_awake(true);
    s_tick = lv_timer_create(tick, 400, NULL);
    paint();
}
