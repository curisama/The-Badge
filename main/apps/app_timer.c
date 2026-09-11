/* The timer. One of the things a round screen does best — the time left is
 * watched as a shrinking ring rather than read as a number. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"

static const int PRESET[3] = { 5, 15, 25 };     /* minutes */

static lv_obj_t   *s_arc, *s_time, *s_hint;
static lv_obj_t   *s_chip[3];
static lv_timer_t *s_tick;

static int  s_total;        /* seconds */
static int  s_left;
static bool s_running;
/* A Casio-style alarm. Two short 4 kHz notes, a rest, two more — four groups.
 * 1 = sound, 0 = silence. One slot is 70 ms. */
static const uint8_t BEEP[] = {
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
};
#define BEEP_HZ   4000
#define BEEP_STEP 70
static int s_beep_i = -1;   /* -1 = not ringing */
static uint32_t s_press_ms;
static bool     s_long_done;

static void paint(void)
{
    /* Stopped, the ring shows "the minutes set"; running, "how much is left" */
    if (s_running || s_left != s_total) {
        lv_arc_set_range(s_arc, 0, 1000);
        lv_arc_set_value(s_arc, s_total ? s_left * 1000 / s_total : 0);
    } else {
        lv_arc_set_range(s_arc, 1, 90);
        lv_arc_set_value(s_arc, s_total / 60);
    }
    lv_label_set_text_fmt(s_time, "%d:%02d", s_left / 60, s_left % 60);

    const char *h = s_left == 0 ? (s_beep_i >= 0 ? "tap to stop" : "done")
                  : (s_running ? "tap to pause" : "tap to start - hold to reset");
    lv_label_set_text(s_hint, h);

    lv_color_t c = s_left == 0 ? lv_color_hex(0xFF6B6B)
                 : s_running   ? lv_color_hex(0x5BD48A)
                               : lv_color_hex(0x7FB0FF);
    lv_obj_set_style_arc_color(s_arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_time, c, 0);
}

/* A one-second timer cannot play the groups. A separate fast timer does it. */
static void beep_step(lv_timer_t *t)
{
    if (s_beep_i < 0) {
        port_tone_enable(false);
        lv_timer_delete(t);
        return;
    }
    /* 🚨 It used to play the groups once and stop by itself (09-08 on the
     * hardware: only four beeps). An alarm has to ring until somebody stops
     * it. The groups start again from the top. */
    if (s_beep_i >= (int)sizeof(BEEP)) s_beep_i = 0;
    port_tone_freq(BEEP_HZ);
    port_tone_enable(BEEP[s_beep_i] != 0);
    s_beep_i++;
}

static void beep_start(void)
{
    if (s_beep_i >= 0) return;
    s_beep_i = 0;
    port_tone_hold(true);        /* hold the codec while it cries so the first note is not lost */
    /* 🚨 keep_awake only stops the screen turning off; it cannot turn on a
     * screen that is already off. Set a 25-minute timer and the screen goes
     * off in the meantime, so the alarm rings in the dark with no way of
     * telling where to press. Turn it on, then hold it on. */
    launcher_screen_on();
    launcher_keep_awake(true);
    lv_timer_create(beep_step, BEEP_STEP, NULL);
}

static void beep_stop(void)
{
    s_beep_i = -1;
    port_tone_enable(false);
    port_tone_hold(false);       /* let the codec go once it stops, back to low power */
}

static void tick(lv_timer_t *t)
{
    (void)t;
    /* Stopped, there is no reason to repaint a 420 px arc every second */
    if (!s_running && s_left != 0 && s_beep_i < 0) return;
    if (s_running && s_left > 0) {
    /* 🚨 Whether to hold used to be decided once, on the tap. Setting 25
     * minutes is not one minute, so it did not hold, and it never looked
     * again. Now it looks every second. */
        if (s_left == 61) launcher_keep_awake(true);
        if (--s_left == 0) {
            s_running = false;
            launcher_keep_awake(false);
            beep_start();
        }
    }
    paint();
}

/* A drag that ends with a lift still sends CLICKED. Check for movement directly. */
static lv_point_t s_press_pt;
static bool       s_dragged;

static void hit_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_press_pt = p;
        s_dragged = false;
        s_press_ms = lv_tick_get();
        s_long_done = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (LV_ABS(p.x - s_press_pt.x) > 8 || LV_ABS(p.y - s_press_pt.y) > 8) s_dragged = true;
        /* A long press resets. It happens on the spot rather than waiting for
         * the lift — that is what gives the finger an answer. */
        if (!s_dragged && !s_long_done && lv_tick_get() - s_press_ms >= 600) {
            s_long_done = true;
            s_running = false;
            s_left = s_total;
            beep_stop();
            launcher_keep_awake(false);
            paint();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_dragged || s_long_done) return; /* a drag or a long press is not a start */
        if (s_left == 0) { s_left = s_total; beep_stop(); }
        else             { s_running = !s_running; }
        /* 🚨 It used to hold the whole way through. Set 90 minutes and the
         * screen stayed on for 90 minutes. But s_tick keeps running with the
         * screen off and the alarm still sounds (screen_off does not suspend
         * app timers). So holding the last minute is enough. */
        launcher_keep_awake(s_running && s_left <= 60);
        paint();
    }
}

/* The ring at the edge sets the time. It cannot be changed while running. */
static void dial_cb(lv_event_t *e)
{
    if (s_running) { lv_arc_set_value(s_arc, s_total ? s_left * 1000 / s_total : 0); return; }
    int min = lv_arc_get_value(lv_event_get_target(e));
    if (min < 1) min = 1;
    s_total = min * 60;
    s_left  = s_total;
    beep_stop();
    lv_label_set_text_fmt(s_time, "%d:%02d", s_left / 60, s_left % 60);
    lv_label_set_text(s_hint, "tap to start");
}

static void preset_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_total = PRESET[i] * 60;
    s_left  = s_total;
    s_running = false;
    beep_stop();
    for (int k = 0; k < 3; k++) {
        lv_obj_set_style_bg_color(s_chip[k],
            lv_color_hex(k == i ? 0x3A5A7A : 0x24242A), 0);
    }
    paint();
}

void timer_build(lv_obj_t *root)
{
    s_total = PRESET[2] * 60;
    s_left  = s_total;
    s_running = false;
    s_beep_i = -1;

    s_arc = lv_arc_create(root);
    lv_obj_set_size(s_arc, 420, 420);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 1, 90);
    lv_arc_set_value(s_arc, s_total / 60);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1E1E22), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 10, LV_PART_KNOB);
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_ADV_HITTEST);   /* responds only on the ring band */
    lv_obj_add_event_cb(s_arc, dial_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* The whole middle is the press target. No need to aim at a small button. */
    lv_obj_t *hit = lv_button_create(root);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 250, 170);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_RELEASED, NULL);

    s_time = lv_label_create(root);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -30);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6E6E72), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 18);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = lv_button_create(root);
        lv_obj_set_size(c, 74, 44);
        lv_obj_set_style_radius(c, 22, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(i == 2 ? 0x3A5A7A : 0x24242A), 0);
        lv_obj_set_style_shadow_width(c, 0, 0);
        lv_obj_align(c, LV_ALIGN_CENTER, (i - 1) * 84, 120);
        lv_obj_add_event_cb(c, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text_fmt(l, "%d", PRESET[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
        s_chip[i] = c;
    }

    s_tick = lv_timer_create(tick, 1000, NULL);
    paint();
}

void timer_free(void)
{
    launcher_keep_awake(false);
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    beep_stop();
    s_running = false;      /* leaving stops it. This is not a thing to run in the background */
}

