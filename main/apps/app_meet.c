/* Meeting recording. The badge records on its own.
 *
 * A company meeting room has no WiFi and no moment to pull out a phone. So
 * this app connects to nothing — it opens the microphone and piles the audio
 * into flash. Getting the files off is a deliberate act: hold the button, the
 * badge re-enumerates as a read-only USB drive, and you copy them.
 *
 * Recording carries on with the app closed and the screen off (a separate
 * task). That is what lets the badge sit in a pocket through a meeting. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>

typedef enum { M_IDLE, M_REC, M_SAVED, M_FAIL } mstate_t;

static lv_obj_t   *s_ring, *s_btn, *s_big, *s_sub, *s_hint;
static lv_timer_t *s_tick;
static mstate_t    s_st;
static bool        s_english;
static const char *s_why;

static void paint(void)
{
    static const uint32_t COL[] = {
        0x7FB0FF,   /* IDLE  blue */
        0xFF5B5B,   /* REC   red */
        0x5BD48A,   /* SAVED green */
        0xFF6B6B,   /* FAIL  red */
    };
    lv_color_t c = lv_color_hex(COL[s_st]);

    char big[16], sub[40];
    const char *hint = "";
    int secs = (int)port_rec_seconds();

    switch (s_st) {
    case M_IDLE: {
        snprintf(big, sizeof big, "REC");
        uint32_t freem = port_rec_free_seconds() / 60;
        int pend = port_rec_pending();
        if (pend > 0) snprintf(sub, sizeof sub, "%d saved", pend);
        else          snprintf(sub, sizeof sub, "%lu min free", (unsigned long)freem);
        hint = "hold = export";
        break;
    }
    case M_REC:
        snprintf(big, sizeof big, "%d:%02d", secs / 60, secs % 60);
        snprintf(sub, sizeof sub, "recording");
        hint = "tap = stop";
        break;
    case M_SAVED:
        snprintf(big, sizeof big, "OK");
        snprintf(sub, sizeof sub, "saved  %d on device", port_rec_pending());
        hint = "export over USB";
        break;
    default:
        snprintf(big, sizeof big, "!");
        snprintf(sub, sizeof sub, "%s", s_why ? s_why : "failed");
        hint = "tap = retry";
        break;
    }

    lv_label_set_text(s_big, big);
    lv_label_set_text(s_sub, sub);
    lv_label_set_text(s_hint, hint);
    lv_obj_set_style_text_color(s_big, c, 0);
    lv_obj_set_style_border_color(s_btn, c, 0);
    lv_obj_set_style_arc_color(s_ring, c, LV_PART_INDICATOR);

    /* While recording the ring goes round once a minute. It stands in for a second hand. */
    lv_arc_set_value(s_ring, s_st == M_REC ? (secs % 60) * 100 / 60 : 100);
}

static void go(mstate_t st)
{
    s_st = st;
    /* The screen must not be held while recording. An hour of 466x466 AMOLED
     * costs far more than the microphone. Recording runs in a task, so the
     * screen has nothing to do with it. */
    launcher_keep_awake(false);
    paint();
}

static void start(bool english)
{
    s_english = english;
    if (!port_rec_start(english ? 1 : 0)) {
        s_why = port_rec_capacity() ? "no space / mic failed" : "no recording partition";
        go(M_FAIL);
        return;
    }
    go(M_REC);
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    switch (s_st) {
    case M_IDLE: start(false); break;
    case M_REC:   port_rec_stop(); go(M_SAVED); break;
    case M_SAVED:
    case M_FAIL:  go(M_IDLE);                   break;
    }
}

static void long_cb(lv_event_t *e)
{
    (void)e;
    if (s_st == M_IDLE) start(true);
}

static void tick(lv_timer_t *t)
{
    /* 🔋 Screen off, nobody is looking. But 🚨 the period must not be changed
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so calling it from a timer callback sends the handler round again on the
     * spot, forever.
     * (09-09: added to save power, and it pinned the CPU at 100%. Setting the
     *  same value does it too, so "only set it when it changes" does not help.)
     * The period stays as it is and the work happens on one tick in six. Same
     * effect, and safe. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 6) return;
    }

    /* If the recording stopped by itself (out of room) the screen follows */
    if (s_st == M_REC && !port_rec_active()) { go(M_SAVED); return; }
    /* Idle, nothing changes. It used to invalidate 86% of the screen twice a
     * second for nothing — it draws only while recording (the elapsed time moves). */
    if (s_st == M_REC) paint();
}

/* 🚨 Export opens **only while stopped**. Slipping into USB mode mid-recording
 * makes the reboot the "end" and cuts off what was being held. */
static void usb_cb(lv_event_t *e)
{
    (void)e;
    if (port_rec_active()) return;
    usb_screen_open();
}

static void meet_enter(lv_obj_t *root)
{
    port_crumb(CRUMB_MEET);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0A0D14), 0);

    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, 430, 430);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 100);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x1E2634), LV_PART_MAIN);

    /* The big button in the middle. On a round screen this is the easiest thing to press. */
    s_btn = lv_obj_create(root);
    lv_obj_set_size(s_btn, 300, 300);
    lv_obj_center(s_btn);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x141A26), 0);
    lv_obj_set_style_border_width(s_btn, 4, 0);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn, tap_cb,  LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn, long_cb, LV_EVENT_LONG_PRESSED,  NULL);

    s_big = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_big, &lv_font_montserrat_48, 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -18);

    s_sub = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0xB6C2D6), 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 34);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A6478), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 178);

    /* The door for taking it off by cable. 🚨 Overlapping the big centre
     * button would have this pressed instead of record — it moves to the top
     * corner (inside the circle, dy -168, dx 108). */
    lv_obj_t *ub = lv_button_create(root);
    lv_obj_set_size(ub, 64, 40);
    lv_obj_set_style_radius(ub, 20, 0);
    lv_obj_set_style_bg_color(ub, lv_color_hex(0x1B2333), 0);
    lv_obj_set_style_shadow_width(ub, 0, 0);
    lv_obj_align(ub, LV_ALIGN_CENTER, 108, -168);
    lv_obj_add_event_cb(ub, usb_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ul = lv_label_create(ub);
    lv_label_set_text(ul, LV_SYMBOL_DRIVE);
    lv_obj_set_style_text_color(ul, lv_color_hex(0x9FB3CC), 0);
    lv_obj_center(ul);

    /* Leaving and coming back returns to that screen if a recording is running */
    s_st = port_rec_active() ? M_REC : M_IDLE;
    s_why = NULL;
    s_tick = lv_timer_create(tick, 500, NULL);
    paint();
}

static void meet_leave(void)
{
    launcher_keep_awake(false);
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    /* Recording is not stopped. It has to keep piling up while other apps are open. */
}

static lv_color_t meet_tint(void) { return lv_color_hex(0xFF5B5B); }

const badge_app_t app_meet = {
    .name = "Meet", .art = &app_icon_meet, .icon = LV_SYMBOL_AUDIO,
    .tint = meet_tint, .radio = RADIO_OFF, .keep_awake = false,
    .enter = meet_enter, .leave = meet_leave,
};
