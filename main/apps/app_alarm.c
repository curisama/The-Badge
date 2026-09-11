/* The alarm. It is one page of the clock app, but **its body lives outside
 * the app.**
 *
 * 🚨 Kept inside the app it would die the moment the clock app closed. The app
 * will not be open when the alarm is due, so that would not be an alarm at
 * all. Watching the time (alarm_tick) is therefore the launcher's job, once a
 * second, and the statics in this file hold everything in between — only the
 * LVGL pieces go away when the app closes; the state stays.
 *
 * 🚨 This board has no RTC chip (0x51 does not answer). Cut the power
 * completely and the time is lost, back to 1970. So **it does not ring until
 * the clock has been set** — judging "7 in the morning" against 1970 would
 * have it going off the moment it is plugged in.
 *
 * 🚨 keep_awake only stops the screen turning off; it cannot turn on a screen
 * that is already off. The timer was caught by exactly this once — turn it on
 * first, then hold it on. */
#include "app.h"
#include "port.h"
#include <time.h>

/* A Casio-style alarm. Same sound as the timer — one sound throughout the badge. */
static const uint8_t BEEP[] = {
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
};
#define BEEP_HZ   4000
#define BEEP_STEP 70

/* ── what survives closing the app ─────────────────────────── */
static uint8_t s_h = 7, s_m = 0;
static bool    s_on;
static bool    s_loaded;
static int     s_fired_yday = -1;   /* has it already rung today — once a day */
static int     s_beep_i = -1;       /* -1 = not ringing */
static lv_obj_t   *s_ring;          /* the panel that covers the screen while ringing */
static lv_timer_t *s_beep_timer;

/* ── what exists only while the app is open ───────────────── */
static lv_obj_t *s_time_lbl, *s_state_lbl, *s_hint, *s_tog;

typedef struct { uint8_t h, m, on; } saved_t;

static void save(void)
{
    saved_t v = { s_h, s_m, (uint8_t)(s_on ? 1 : 0) };
    port_kv_write("alarm", &v, sizeof v);
}

static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    saved_t v;
    if (port_kv_read("alarm", &v, sizeof v) && v.h < 24 && v.m < 60) {
        s_h = v.h; s_m = v.m; s_on = (v.on != 0);
    }
}

/* ── ringing ────────────────────────────────────────────────── */
static void beep_step(lv_timer_t *t)
{
    if (s_beep_i < 0) {
        port_tone_enable(false);
        lv_timer_delete(t);
        s_beep_timer = NULL;
        return;
    }
    /* It cries until somebody stops it. Start the run again from the top. */
    if (s_beep_i >= (int)sizeof(BEEP)) s_beep_i = 0;
    port_tone_freq(BEEP_HZ);
    port_tone_enable(BEEP[s_beep_i] != 0);
    s_beep_i++;
}

static void stop_ring(void);

static void ring_tap_cb(lv_event_t *e)
{
    (void)e;
    stop_ring();
}

static void start_ring(void)
{
    if (s_beep_i >= 0) return;
    s_beep_i = 0;
    port_tone_hold(true);          /* hold the codec while it cries so the first note is not lost */
    launcher_screen_on();          /* 🚨 turn it on first. Holding alone cries in the dark */
    launcher_keep_awake_by(AWAKE_RING, true);

    /* 🚨 It has to cover whichever app is open, so it is built on the top
     * layer — an app deleting its own screen does not delete this. */
    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, 466, 466);
    lv_obj_center(s_ring);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(0x140A0A), 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ring, ring_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *big = lv_label_create(s_ring);
    lv_label_set_text_fmt(big, "%02u:%02u", s_h, s_m);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(big, lv_color_hex(0xFF6B6B), 0);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *h = lv_label_create(s_ring);
    lv_label_set_text(h, "tap to stop");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x8A8A96), 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 50);

    if (!s_beep_timer) s_beep_timer = lv_timer_create(beep_step, BEEP_STEP, NULL);
}

static void stop_ring(void)
{
    s_beep_i = -1;
    port_tone_enable(false);
    port_tone_hold(false);         /* let the codec go once it stops, back to low power */
    launcher_keep_awake_by(AWAKE_RING, false);
    if (s_ring) { lv_obj_delete(s_ring); s_ring = NULL; }
}

/* ── the launcher calls this once a second ──────────────────── */
void alarm_tick(void)
{
    load();
    if (!s_on || s_beep_i >= 0) return;

    /* 🚨 It does not ring until the clock has been set. With no RTC, losing
     * power puts us back in 1970, and judging "7 in the morning" there has it
     * going off the moment it is plugged in. Anything before 2001 counts as
     * unset. */
    time_t now = time(NULL);
    if (now < 978307200) return;

    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_hour != s_h || tm.tm_min != s_m) return;
    if (s_fired_yday == tm.tm_yday) return;     /* once a day */
    s_fired_yday = tm.tm_yday;
    start_ring();
}

/* ── the page ───────────────────────────────────────────────── */
static void paint(void)
{
    if (!s_time_lbl) return;
    lv_label_set_text_fmt(s_time_lbl, "%02u:%02u", s_h, s_m);
    /* 🚨 Off used to be 0x44444E, which sank into the black background and
     * could not be seen (raised 09-10). Off has to be readable too, or there
     * is no telling what it is set to — anything but green will do. */
    lv_obj_set_style_text_color(s_time_lbl,
        s_on ? lv_color_hex(0x5BD48A) : lv_color_hex(0xA8AEBC), 0);
    lv_label_set_text(s_state_lbl, s_on ? "on" : "off");
    lv_obj_set_style_text_color(s_state_lbl,
        s_on ? lv_color_hex(0x081A10) : lv_color_hex(0xD2D8E4), 0);
    /* Show on/off through the panel colour rather than the text — a block of
     * colour reads faster than three letters on a round screen. */
    if (s_tog) {
        lv_obj_set_style_bg_color(s_tog,
            s_on ? lv_color_hex(0x5BD48A) : lv_color_hex(0x30303C), 0);
        lv_obj_set_style_border_color(s_tog,
            s_on ? lv_color_hex(0x8BEBB2) : lv_color_hex(0x4A4A5A), 0);
    }
}

static void bump_cb(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    load();
    switch (what) {
        case 0: s_h = (uint8_t)((s_h + 1) % 24); break;
        case 1: s_h = (uint8_t)((s_h + 23) % 24); break;
        case 2: s_m = (uint8_t)((s_m + 5) % 60); break;
        case 3: s_m = (uint8_t)((s_m + 55) % 60); break;
        case 4: s_on = !s_on; break;
    }
    /* Changing the time also clears "rang today" — so that setting it to a
     * moment just past does not leave somebody asking why it is silent. */
    s_fired_yday = -1;
    save();
    paint();
}

/* 🚨 The panel colour is 0x1D1D24, which left the buttons invisible against
 * the black background (raised 09-10). This screen is aimed at by eye, so the
 * borders are drawn in to mark the edges — and the finger targets grew to
 * 96×72 (from 78×58). */
static lv_obj_t *mk_btn(lv_obj_t *root, int dx, int dy, const char *txt, int what)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, 96, 72);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x30303C), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x4A4A5A), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(b, bump_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD2D8E4), 0);
    lv_obj_center(l);
    return b;
}

void alarm_build(lv_obj_t *root)
{
    load();

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    s_time_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -4);

    /* Hours ▲▼ on the left, minutes ▲▼ on the right, with the number between
     * them so that position alone says what each pair changes — on a round
     * screen there is no room to spare for words.
     * 🚨 Positions are worked out inside the circle. The furthest corner
     * (-160,-116) is 198 px from the centre, inside the radius of 233 — check
     * this again every time something grows, or it will be cut off. */
    mk_btn(root, -112, -80, LV_SYMBOL_UP,   0);
    mk_btn(root, -112,  62, LV_SYMBOL_DOWN, 1);
    mk_btn(root,  112, -80, LV_SYMBOL_UP,   2);
    mk_btn(root,  112,  62, LV_SYMBOL_DOWN, 3);

    /* 🚨 With the toggle on the same row as the arrows it overlapped them by
     * 7 px on each side (09-10, in the simulator). It moves one row down —
     * the furthest corner is 198 px from the centre, so it is safe. */
    s_tog = mk_btn(root, 0, 152, "", 4);
    lv_obj_set_size(s_tog, 150, 62);
    s_state_lbl = lv_label_create(s_tog);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_state_lbl);

    /* 🚨 Putting "hour        min" in a single label pushed both words toward
     * the middle, so they sat over the number instead of over the button
     * columns (09-10, in the simulator). One label per column — a name has to
     * be directly above what it names. */
    s_hint = lv_label_create(root);
    lv_label_set_text(s_hint, "hour");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6E7686), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, -112, -148);

    lv_obj_t *h2 = lv_label_create(root);
    lv_label_set_text(h2, "min");
    lv_obj_set_style_text_font(h2, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(h2, lv_color_hex(0x6E7686), 0);
    lv_obj_align(h2, LV_ALIGN_CENTER, 112, -148);

    paint();
}

void alarm_free(void)
{
    /* 🚨 Only the pieces go. Ringing and the set time live outside the app and
     * are left alone — calling stop_ring here would silence the alarm the
     * moment the clock app closed. */
    s_time_lbl = s_state_lbl = s_hint = s_tog = NULL;
}
