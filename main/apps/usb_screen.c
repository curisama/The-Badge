/* The "export recordings" screen.
 *
 * 🚨 While this is up, the badge's USB-C is not a serial port. That makes
 * this the only door that changes the mode, and the way out has to stay
 * visible — walking in here by accident and not finding the exit leaves you
 * with something you cannot even reflash.
 *
 * 🚨 Done means reboot (see usb_msc.c). Say so on screen: a device that
 * blinks off and back on without warning reads as broken. */
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

    /* Once it is up the badge is a drive. Show whether a host actually
     * attached, so "I turned it on and nothing appeared" separates a cable
     * problem from a badge problem. */
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
    /* If the host ejects it properly, switch back to serial right there —
     * no reason to make anyone touch the badge again. */
    if (usb_msc_active() && usb_msc_ejected()) { paint(); usb_msc_stop(); return; }
    paint();
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    if (usb_msc_active()) { usb_msc_stop(); return; }   /* done — this reboots */
    if (usb_export_files() <= 0) return;
    if (!usb_msc_start()) lv_label_set_text(s_note, "could not switch to USB");
    paint();
}

static void usb_screen_close(void)
{
    /* 🚨 No way out once the drive is up: leaving means changing the mode
     * back, and that means rebooting. Rather than lock the door, do it. */
    if (usb_msc_active()) { usb_msc_stop(); return; }
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    launcher_keep_awake(false);
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_btn = s_big = s_sub = s_note = NULL;
}

void usb_screen_open(void)
{
    if (s_scr) return;
    usb_export_build();              /* lay out the volume from what is stored */

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
    /* 🚨 The display must not sleep mid-export. There is no way to press done
     * on a dark screen, and pulling the cable in that state makes Windows
     * complain. */
    launcher_keep_awake(true);
    s_tick = lv_timer_create(tick, 400, NULL);
    paint();
}
