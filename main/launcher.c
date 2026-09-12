#include "app.h"
#include "port.h"
#include "display.h"
#include "assets/assets.h"
#include "nightsky.h"
#include <math.h>

static const char *TAG = "launcher";

/* Screen diameter. It is round, so anything in a corner gets cut off. */
#define SCREEN_D        466
/* More apps means overlapping tiles. Shrink them to fit the count. */
#define RING_R          (APP_CNT <= 4 ? 122 : (APP_CNT <= 5 ? 132 : 150))
#define ICON_D          (APP_CNT <= 4 ? 126 : (APP_CNT <= 5 ? 120 : 92))

static lv_obj_t          *s_home;
static lv_obj_t          *s_batt;
static int                s_prev_p = -2;   /* stops the battery text redrawing; reset when home is rebuilt */
static bool               s_prev_plug;
static lv_obj_t          *s_app_scr;
static const badge_app_t *s_current;

/* ── turning the display off ──────────────────────────────────
 * AMOLED plus a small battery: without this it empties in hours.
 * The tap that wakes it has to be swallowed, or waking also presses whatever
 * icon was under your finger. */
static lv_obj_t   *s_veil;          /* the black sheet laid over a sleeping screen */
static int         s_crumb_before_off = -1;
static bool        s_touch_wakes;   /* can a touch wake it? */
/* An app can hold it awake for a while (a timer counting down, say) */
static bool        s_awake_hold;

/* 🚨 Several things can ask to stay awake at once — a timer alarm going off
 * while the stopwatch runs. It used to be that whichever let go first
 * cancelled the others as well. Count the holders and only release when the
 * last one lets go. */
static uint8_t s_awake_bits;
void launcher_keep_awake_by(int who, bool on)
{
    if (who < 0 || who > 7) return;
    if (on) s_awake_bits |= (uint8_t)(1u << who);
    else    s_awake_bits &= (uint8_t)~(1u << who);
    s_awake_hold = (s_awake_bits != 0);
}
void launcher_keep_awake(bool on) { launcher_keep_awake_by(AWAKE_APP, on); }
static lv_timer_t *s_idle;
static lv_timer_t *s_rotate_timer, *s_heap_timer, *s_batt_timer;
static bool  s_autorotate = false;

static int         s_timeout_s = 30;

static void screen_wake(void);

/* ── saving power with the display off ────────────────────────
 * The CPU keeps waking even with the display off, and the most frequent
 * reason is reading touch: every 12 ms, or 83 I2C transactions a second. But
 * this device **does not wake on touch** whether it slept by itself or you
 * turned it off (pocket protection — see idle_cb). So there is no reason at
 * all to read touch while it is off. Stop.
 * This is nothing like as dangerous as light sleep. It only pauses timers and
 * starts them again, so the worst case is "touch is a beat late on wake". */
static void idle_timers(bool screen_on)
{
    /* 🚨 The big one: LVGL's refresh timer runs every 33 ms (30 a second)
     * whether or not anything needs drawing. With the display off there is
     * nothing to draw, but it keeps waking the CPU — so light sleep pays the
     * cost of entering and leaving without ever sleeping properly.
     * Pausing it makes the longest remaining gap 200 ms (the button), which
     * is deep enough to matter. */
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_timer_t *rt = lv_display_get_refr_timer(d);
        if (rt) { if (screen_on) lv_timer_resume(rt); else lv_timer_pause(rt); }
    }
    /* Checking for idleness is meaningless while off — it is already off. */
    if (s_idle) { if (screen_on) lv_timer_resume(s_idle); else lv_timer_pause(s_idle); }

    lv_indev_t *in = badge_display_indev();
    if (in) {
        lv_timer_t *rt = lv_indev_get_read_timer(in);
        if (rt) {
            if (screen_on) { lv_timer_resume(rt); lv_timer_set_period(rt, 12); }
            else             lv_timer_pause(rt);
        }
    }
    if (s_rotate_timer) {
        /* Auto-rotate is off by default, and there is no reason to wake every 200 ms for it. */
        if (screen_on && s_autorotate) lv_timer_resume(s_rotate_timer);
        else                           lv_timer_pause(s_rotate_timer);
    }
    if (s_heap_timer)  lv_timer_set_period(s_heap_timer,  screen_on ? 10000 : 120000);
    if (s_batt_timer) {
        /* No reason to repaint an invisible label every 20 seconds */
        if (screen_on) lv_timer_resume(s_batt_timer);
        else           lv_timer_pause(s_batt_timer);
    }
}

static void wake_cb(lv_event_t *e)
{
    (void)e;
    screen_wake();
}

static void screen_off(bool touch_wakes)
{
    if (s_veil) return;
    /* 🚨 Write the battery journal line *just before* turning off. This line
     * and the one written on wake sit next to each other, and the gap between
     * them is "the display was on". Writing it after turning off records
     * screen-off and breaks the pair — which is why screen-on consumption had
     * never once been measured. */
    port_battery_mark(true);
    s_touch_wakes = touch_wakes;
    /* 🚨 Capture what was being counted just before going dark. Unlocking
     * back into that app has to continue its per-app total, or the whole
     * stretch lands under "lock screen". */
    s_crumb_before_off = port_crumb_now();
    port_crumb(CRUMB_SCR_OFF);
    port_log(TAG, "screen OFF (touch_wakes=%d)", touch_wakes);
    if (!touch_wakes) idle_timers(false);   /* if touch cannot wake it, there is no reason to read it */
    lock_stop();
    port_display_power(false);
    /* Covering the top layer means nothing underneath can be reached by a finger */
    s_veil = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_veil);
    lv_obj_set_size(s_veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_veil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_veil, LV_OPA_COVER, 0);
    /* Turned off by hand means touch is ignored. In a pocket the lightest
     * brush would wake it and melt the battery — the same rule as a phone's
     * lock button. Either way the veil itself blocks what is below. */
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);
    if (touch_wakes) lv_obj_add_event_cb(s_veil, wake_cb, LV_EVENT_PRESSED, NULL);
}

void launcher_screen_off(void)        { screen_off(true);  }
void launcher_screen_off_manual(void) { screen_off(false); }

bool launcher_screen_is_off(void) { return s_veil != NULL; }

static void screen_wake(void)
{
    if (!s_veil) return;
    /* 🚨 This is the scene of "turning the display off and on reboots it".
     * Written just before waking, so a boot record showing screen-on points
     * straight at this path. */
    /* Nothing is recorded here. The lock screen or the app writes its own
     * name immediately after — recording screen-on here would stick and ruin
     * the per-app totals. */
    idle_timers(true);
    lv_obj_delete(s_veil);
    s_veil = NULL;
    port_display_power(true);
    port_log(TAG, "screen ON");
    /* 🚨 It used to return straight to whatever app was open. That made two
     * rules: wake from home and you get the lock screen, wake from an app and
     * you get the app. Always the lock screen now — this is used like a
     * watch, and it also cuts down on nudging an app in your pocket. The app
     * is not destroyed; it is still alive behind, and pulling the handle
     * returns you to it (launcher_unlock, called from lock.c). */
    launcher_show_lock();
    /* Waking has to reset the idle clock too, or the next check a second later
     * still sees "thirty seconds have passed" and turns it straight back off. */
    lv_display_trigger_activity(NULL);
    /* Start of a display-on stretch. 🚨 Called after the display is on —
     * reading the voltage over I2C takes long enough that doing it first
     * makes the screen visibly late. */
    port_battery_mark(true);
}

/* Holding PWR makes the AXP2101 cut power a few seconds later. We cannot stop
 * that, but the press should be acknowledged or it looks broken. */
void launcher_poweroff_notice(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, "power off");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9A9A9E), 0);
    lv_obj_center(l);
}

/* The home screen is a bright wallpaper with a lot of lit pixels. Waking goes to the lock screen, which is black. */
void launcher_show_home(void)
{
    /* The journal splits per-app consumption by "what is on screen now".
     * Without this, unlocking still reads as screen-on and the whole home
     * stretch is attributed wrongly. */
    port_crumb(CRUMB_HOME);
    lock_stop();
    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

/* Unlock. 🚨 Going to home would interrupt whatever was happening — the
 * stopwatch might be running, or the mouse in use. If the app is alive, go back to it. */
void launcher_unlock(void)
{
    if (s_app_scr && lv_obj_is_valid(s_app_scr)) {
        if (s_crumb_before_off >= 0) port_crumb(s_crumb_before_off);
        lock_stop();
        lv_screen_load_anim(s_app_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
        return;
    }
    launcher_show_home();
}

void launcher_show_lock(void)
{
    port_crumb(CRUMB_LOCK);
    lv_obj_t *lk = lock_screen();
    lock_start();
    lv_screen_load_anim(lk, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

/* Wake the display when something needs a person right now, like an alarm.
 * keep_awake only stops it sleeping; it cannot wake a screen already off. */
void launcher_screen_on(void)
{
    if (s_veil) screen_wake();
}

void launcher_screen_toggle(void)
{
    if (s_veil) screen_wake();
    else        launcher_screen_off_manual();
}

static void idle_cb(lv_timer_t *t)
{
    (void)t;
    /* 🚨 The alarm lives outside its app. It has to ring with the Clock app
     * closed and with the display off, so it is checked here every second
     * rather than there.
     * It has to sit **above** the early returns below — with the display off
     * (s_veil) nothing further down is reached. */
    alarm_tick();
    if (s_timeout_s <= 0 || s_veil) return;
    /* With the display off the veil blocks touch, which kills the mouse
     * outright. It is also used while looking at another screen, so idle time
     * means nothing for it. */
    if (s_current && s_current->keep_awake) return;
    if (s_awake_hold) return;
    if (lv_display_get_inactive_time(NULL) > (uint32_t)s_timeout_s * 1000) {
        /* Sleeping by itself does not make touch a wake source either. A
         * brush in a pocket would melt the battery. The button wakes it. */
        launcher_screen_off_manual();
    }
}

void launcher_set_timeout(int seconds) { s_timeout_s = seconds; }
int  launcher_get_timeout(void)        { return s_timeout_s; }

/* Home is two pages. Six icons on one page shrinks them to 92 px, which is
 * hard to hit; split in two they are 120 px. Only redrawn when you turn the
 * page, so sitting still costs nothing. */
static const badge_app_t *const s_page0[] = { &app_games, &app_orb, &app_water,
                                              &app_air, &app_clock, &app_calc };
static const badge_app_t *const s_page1[] = { &app_mouse, &app_keys, &app_meet };
#define PAGE_N 2
static const badge_app_t *const *const s_pages[PAGE_N] = { s_page0, s_page1 };
static const uint8_t s_page_cnt[PAGE_N] = {
    (uint8_t)(sizeof(s_page0) / sizeof(s_page0[0])),
    (uint8_t)(sizeof(s_page1) / sizeof(s_page1[0])),
};
static uint8_t s_page;          /* the page being shown */

#define s_apps   (s_pages[s_page])
#define APP_CNT  ((size_t)s_page_cnt[s_page])

static lv_timer_t *s_ble_off_timer;

static void ble_off_cb(lv_timer_t *t)
{
    (void)t;
    s_ble_off_timer = NULL;
    port_hid_stop();
}

/* ── radio state ──────────────────────────────────────────────
 * WiFi and BLE share the antenna. Neither is on right now, so this only logs;
 * the real on/off goes here when BLE HID attaches. */
static void radio_apply(radio_need_t need)
{
    static radio_need_t cur = RADIO_OFF;
    if (cur == need) return;
    cur = need;
    port_radio_set((int)need);

    /* Dropping BLE the instant an app closes makes the badge vanish from the
     * phone's list and disconnects it. Doing that every time you dip in and
     * out of an app is unusable. There is a 45-second grace period, and
     * re-entering a BLE app within it cancels the whole thing. */
    if (need == RADIO_BLE) {
        if (s_ble_off_timer) { lv_timer_delete(s_ble_off_timer); s_ble_off_timer = NULL; }
        port_hid_start();
    } else if (!s_ble_off_timer) {
        s_ble_off_timer = lv_timer_create(ble_off_cb, 45000, NULL);
        lv_timer_set_repeat_count(s_ble_off_timer, 1);
    }
}


/* The battery journal. The badge writes this itself so that a night left with
 * the display off still leaves a record — with no cable there is nowhere for
 * serial logs to go. */
static void batt_log_cb(lv_timer_t *t)
{
    (void)t;
    port_battery_log(s_veil ? "idle-off" : "idle-on");
    port_imu_idle_check();

    /* 🚨 Do not let it sleep while plugged in.
     * USB-Serial-JTAG stops when the CPU does, so the moment light sleep
     * engages the log cuts out entirely and there is no telling whether the
     * device is fine or dead (two verification runs were lost to this).
     * Plugged in, there is no battery to save anyway. */
    static bool held;
    bool plugged = port_battery_plugged();
    if (plugged != held) {
        port_pm_hold(plugged);
        /* 🚨 Plugging in is the badge arriving somewhere, and somewhere is
         * usually where its WiFi is. Housekeeping runs every thirty minutes,
         * so without this a day out with the clock drifting can go another
         * half hour after you are home. port_time_autosync() decides for
         * itself whether it is due, so this costs nothing when it is not. */
        if (plugged) port_time_autosync();
        held = plugged;
    }
    port_uptime_mark();
}

static void housekeep_cb(lv_timer_t *t)
{
    (void)t;
    if (port_rec_active()) return;      /* leave it alone while recording */
    /* 🚨 The clock is handled here too. It used to be set once at boot, and
     * only when there was no time at all — so once set it was never set
     * again, and a board with no RTC drifts minutes a day (3-4 minutes ahead
     * of a phone). Calling this is free: if less than an hour has passed it
     * returns immediately. */
    port_time_autosync();
}

/* ── turning pages ────────────────────────────────────────────
 * 🚨 LVGL gestures were tried and abandoned. A gesture only goes to the
 * object that was pressed; sending it upward needs GESTURE_BUBBLE rather than
 * EVENT_BUBBLE; and if the screen can scroll, the swipe is taken as a scroll
 * and never delivered at all. Three conditions, and missing any one of them
 * means nothing happens, silently (three wrong guesses in a row).
 * Measuring where the finger landed and where it lifted has none of that. A
 * large sideways movement turns the page, and that press does not open an app. */
#define SWIPE_PX 60
static void build_home(void);
void launcher_show_home(void);
static int32_t s_press_x;
static bool    s_swiped;

static void home_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    s_press_x = p.x;
    s_swiped = false;
}

/* Returns true if the page turned — the caller must then not open an app */
static bool home_release_check(void)
{
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    int32_t dx = p.x - s_press_x;
    if (dx > -SWIPE_PX && dx < SWIPE_PX) return false;
    uint8_t was = s_page;
    if (dx < 0) s_page = (uint8_t)((s_page + 1) % PAGE_N);
    else        s_page = (uint8_t)((s_page + PAGE_N - 1) % PAGE_N);
    s_swiped = true;
    if (was != s_page) {
        /* 🚨 launcher_show_home() only re-shows a screen that already exists.
         * A page change means different tiles, so home has to be rebuilt —
         * otherwise the internal value changes and the screen does not. */
        lv_obj_t *old = s_home;
        build_home();
        lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, 160, 0, false);
        if (old) lv_obj_delete_delayed(old, 400);   /* clean it up after the transition */
    }
    return true;
}

static void home_swipe_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { home_press_cb(e); return; }
    home_release_check();
}

static void app_btn_cb(lv_event_t *e)
{
    /* If this was a sideways swipe that turned the page, do not open an app */
    if (home_release_check()) return;
    launcher_open((const badge_app_t *)lv_event_get_user_data(e));
}

/* An app can hold the display awake for a while — a timer counting down, say,
 * where the screen has to stay alive without being touched. */
/* ── auto-rotate ──────────────────────────────────────────────
 * Rotating the whole display means no app has to twist its own coordinates.
 *
 * Three rules keep it from being dizzying:
 *   · it snaps to 90-degree steps and never follows an in-between angle
 *   · it needs 60 degrees past the boundary to commit (ignores wrist wobble)
 *   · and that attitude has to hold for 0.8 s. It never turns under a finger.
 * The sensor is read at 5 Hz, so the power cost is effectively nothing. */
/* Off by default. Board logs showed 500 mg of in-plane component while lying
 * flat (how the sensor axes line up with the display is still not pinned
 * down), so it turned on its own. On top of that it was never confirmed that
 * touch coordinates rotate with the display — if they do not, nothing can be
 * pressed. Turn it on in Settings, confirm, then make it the default. */
static int   s_rot;             /* 0~3 */
static int   s_rot_cand;
static int   s_rot_hold;

void launcher_set_autorotate(bool on)
{
    s_autorotate = on;
    /* With it off there is no reason to run a 200 ms timer. Restarted when it is enabled. */
    if (s_rotate_timer) {
        if (on && !s_veil) lv_timer_resume(s_rotate_timer);
        else               lv_timer_pause(s_rotate_timer);
    }
}
bool launcher_get_autorotate(void)    { return s_autorotate; }

static void rotate_poll(lv_timer_t *t)
{
    (void)t;
    if (!s_autorotate || s_veil) return;

    lv_indev_t *in = lv_indev_get_next(NULL);
    if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) return;  /* never under a finger */

    float deg;
    if (!port_imu_angle(&deg)) return;
    /* 🚨 This code is unverified. The reference orientation was flipped 180
     * degrees with MADCTL, and the IMU is bolted to the board and did not
     * follow — so switching auto-rotate back on will probably need 180 added
     * to this angle (the same thing happened with the marble game).
     * Whether touch rotates with it is also still unconfirmed. */
    /* Only act when it is clearly held upright. Rotating while flat is the worst case. */
    if (!port_imu_upright()) return;

    /* Split gravity's direction into four zones. The boundary is at 45
     * degrees, but 15 more are required so it does not flicker there. */
    float a = deg;
    while (a < 0) a += 360.f;
    int want = s_rot;
    for (int k = 0; k < 4; k++) {
        float centre = k * 90.f;
        float d = a - centre;
        while (d > 180.f)  d -= 360.f;
        while (d < -180.f) d += 360.f;
        if (fabsf(d) < 30.f) { want = k; break; }     /* within 30 degrees of a zone's centre */
    }

    if (want != s_rot_cand) { s_rot_cand = want; s_rot_hold = 0; return; }
    if (want == s_rot) return;
    if (++s_rot_hold < 4) return;                     /* 5 Hz x 4 = held for 0.8 s */

    s_rot = want;
    s_rot_hold = 0;
    lv_display_set_rotation(lv_display_get_default(),
        (lv_display_rotation_t)(LV_DISPLAY_ROTATION_0 + s_rot));
    port_log(TAG, "display rotated %d degrees", s_rot * 90);
}

static void heap_cb(lv_timer_t *t)
{
    (void)t;
    port_heap_report(s_current ? s_current->name : "idle");
}

static void batt_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_batt) return;
    int p = port_battery_percent();
    if (p < 0) {
    lv_label_set_text(s_batt, "");           /* with no battery, show nothing at all */
        return;
    }
    bool plug = port_battery_plugged();

    /* 🔋 Do nothing if the value has not changed. It used to rewrite the label
     * every 20 seconds regardless, redrawing that area for a number that was
     * the same. With this guard it draws less than before even when checked
     * more often.
     * 🚨 But a rebuilt home screen has a new label, so it has to be written at
     * least once — otherwise LVGL's placeholder text ("Text") is what stays.
     * build_home clears this. */
    if (p == s_prev_p && plug == s_prev_plug) return;
    s_prev_p = p;
    s_prev_plug = plug;

    lv_label_set_text_fmt(s_batt, plug ? "%d%% +" : "%d%%", p);

    /* Ten colour steps from red to green. The colour tells you roughly where
     * you are before you read the number. */
    static const uint32_t LEVEL[11] = {
        0xE83B3B,  /*   0~9  */
        0xE85C3B,  /*  10~19 */
        0xE8783B,  /*  20~29 */
        0xE8963B,  /*  30~39 */
        0xE8B43B,  /*  40~49 */
        0xE0CC3B,  /*  50~59 */
        0xC9D63A,  /*  60~69 */
        0xA8D44A,  /*  70~79 */
        0x86D25C,  /*  80~89 */
        0x66D073,  /*  90~99 */
        0x4FCF8A,  /*  100   */
    };
    int idx = p / 10;
    if (idx > 10) idx = 10;
    uint32_t col = plug ? 0x4FC3F7 : LEVEL[idx];   /* plugged in shows blue */
    lv_obj_set_style_text_color(s_batt, lv_color_hex(col), 0);
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    if (home_release_check()) return;
    launcher_open(&app_settings);
}

static void build_home(void)
{
    s_batt = NULL;
    s_home = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_style_bg_color(s_home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);

    /* The wallpaper is drawn, not stored — see nightsky.h. */
    nightsky_create(s_home);

    /* Apps 120 degrees apart. The icons are the app's own screen shrunk down,
     * which says what it is far faster than a glyph. */
    for (size_t i = 0; i < APP_CNT; i++) {
        const badge_app_t *a = s_apps[i];
        float ang = (float)(-M_PI / 2.0 + i * (2.0 * M_PI / APP_CNT));
        int x = (int)(cosf(ang) * RING_R);
        int y = (int)(sinf(ang) * RING_R);

        lv_obj_t *tile = lv_image_create(s_home);
        lv_image_set_src(tile, a->art);
        lv_obj_set_size(tile, ICON_D, ICON_D);
    lv_image_set_scale(tile, ICON_D * 256 / 120);   /* scale the 120 px source to fit */
        lv_image_set_pivot(tile, 60, 60);
        lv_obj_align(tile, LV_ALIGN_CENTER, x, y);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        /* 🚨 A gesture only goes to the object that was pressed. Swiping over
         * an icon means the icon takes it, the screen never sees it, and the
         * page does not turn.
         * LVGL has a separate flag just for gestures — GESTURE_BUBBLE, not
         * EVENT_BUBBLE. With that, presses still go to the icon and only
         * swipes travel up to the screen. */
        lv_obj_add_event_cb(tile, home_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(tile, app_btn_cb, LV_EVENT_RELEASED, (void *)a);

        /* LVGL's built-in font has no bold, so it is drawn twice one pixel
         * apart. The background is a photo, so a dark shadow underneath lifts
         * the text off it. */
        int ny = y + ICON_D / 2 + 21;
        for (int k = 0; k < 3; k++) {
            static const int8_t OX[3] = { 1, 0, 1 };
            static const int8_t OY[3] = { 2, 0, 0 };
            lv_obj_t *nm = lv_label_create(s_home);
            lv_label_set_text(nm, a->name);
            lv_obj_set_style_text_font(nm, APP_CNT <= 5 ? &lv_font_montserrat_20
                                                        : &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(nm, k == 0 ? lv_color_black() : lv_color_hex(0xF2F5F8), 0);
            lv_obj_set_style_text_opa(nm, k == 0 ? 150 : LV_OPA_COVER, 0);
            lv_obj_align(nm, LV_ALIGN_CENTER, x + OX[k], ny + OY[k]);
        }
    }

    /* Page dots — how many pages there are and which one this is. Two dots is enough. */
    for (int k = 0; k < PAGE_N; k++) {
        lv_obj_t *d = lv_obj_create(s_home);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(k == s_page ? 0xE8ECF0 : 0x4A4A52), 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (k - (PAGE_N - 1) * 0.5f) * 18, 196);
    }

    /* Swipe sideways to turn the page.
     * 🚨 Screens are scrollable by default, so LVGL takes the swipe as a
     * scroll and never emits the gesture at all (it returns on the first line
     * of indev_gesture). Turning scrolling off is what lets the gesture through. */
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home, home_swipe_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_home, home_swipe_cb, LV_EVENT_RELEASED, NULL);

    /* Settings is the gear in the middle. A ninth app tile would unbalance the ring. */
    lv_obj_t *gear = lv_image_create(s_home);
    lv_image_set_src(gear, &icon_gear);
    lv_obj_set_style_image_recolor(gear, lv_color_hex(0xD6DEE6), 0);
    lv_obj_set_style_image_recolor_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(gear, 22);
    lv_obj_center(gear);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear, home_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(gear, settings_cb, LV_EVENT_RELEASED, NULL);

    /* Battery — an icon would only take up room. One number is enough, and
     * directly under the gear it is not in the way. */
    s_batt = lv_label_create(s_home);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_20, 0);
    lv_obj_align(s_batt, LV_ALIGN_CENTER, 0, 44);
    s_prev_p = -2;              /* new label, so write it at least once */
    batt_refresh(NULL);
    /* This timer is paused while the display is off (idle_timers), so it only
     * runs when the screen is on — and the CPU is awake for the display
     * anyway. Reading the PMU is two bytes over I2C, which is nothing. */
    /* 🚨 Creating the timer on every home rebuild stacks one up per page
     * turn, and they are never deleted. Clear it first. */
    if (s_batt_timer) lv_timer_delete(s_batt_timer);
    s_batt_timer = lv_timer_create(batt_refresh, 5000, NULL);
}

/* ── open and close animation ────────────────────────────────
 * The screen grows out of the icon and shrinks back into it on the way out.
 * LVGL's built-in screen transitions are all slides, so the app's contents go
 * on a separate object whose scale and opacity are animated directly. */
#define ANIM_MS_OPEN   190
#define ANIM_MS_CLOSE  150

static lv_obj_t *s_stage;
static bool      s_closing;

/* ── the home handle ──────────────────────────────────────────
 * The iPhone bar. Grab it, push up, and you are back at home.
 * It sits on its own above the app's screen, so even in apps that use the
 * whole surface (the mouse trackpad) it only acts **when the finger started
 * on the handle**. A drag through the middle that happens to pass over it is
 * not seen. */
#define SWIPE_UP    45      /* lift it this far and you are out */
/* 🚨 The grab area is defined in exactly one place. When an app handles the
 * handle gesture itself (launcher_handle_zone), a second definition means
 * what you see and what responds are in different places. The visible arc is a
 * separate, deliberately thinner thing — its size is set where it is built. */
#define HANDLE_W    240
#define HANDLE_H    64

static void (*s_handle_action)(void);
static lv_obj_t *s_handle;      /* the visible arc */
static lv_obj_t *s_handle_hit;  /* the invisible pad that actually takes the finger */
static int32_t   s_handle_y0;
static bool      s_handle_armed;

/* 🚨 This used to read the static s_handle. But a handle is built per screen
 * — one on home, one on the lock screen, one on the app screen — and there
 * was only ever one pointer. Closing an app made close_done_cb NULL it while
 * the home screen's handle was alive and still holding the callback, so
 * touching that set a style on NULL and died (caught in a core dump: obj=0x0,
 * lv_obj_set_local_style_prop, doing=screen-on).
 *
 * Each handle now carries its own arc in user_data. Nothing looks at anyone
 * else's pointer, so it does not matter which screen gets deleted. */
static void handle_cb(lv_event_t *e)
{
    lv_obj_t *arc = (lv_obj_t *)lv_event_get_user_data(e);
    if (!arc) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_handle_y0 = p.y;
        s_handle_armed = false;
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dy = p.y - s_handle_y0;
        if (dy > 0) dy = 0;
        if (dy < -60) dy = -60;
        lv_obj_set_style_translate_y(arc, dy, 0);   /* follows the finger */
        if (dy <= -SWIPE_UP) s_handle_armed = true;
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_set_style_translate_y(arc, 0, 0);
        lv_obj_set_style_arc_opa(arc, 150, LV_PART_MAIN);
        /* The action differs per handle too: the lock screen wants
         * launcher_show_home and an app screen wants launcher_home. With one
         * static, whichever was attached last wins. Worse, lock_screen()
         * caches its screen and does not re-attach the handle after the first
         * time, so opening an app once made unlocking call launcher_home —
         * which returns immediately when no app is open, i.e. it never
         * unlocked. So each one uses what is attached to itself. */
        void (*act)(void) = (void (*)(void))lv_obj_get_user_data(arc);
        if (!act) act = s_handle_action;          /* fallback for the old path */
        if (s_handle_armed && act) act();
    }
}

/* In apps that use the bottom of the screen (games), the handle steals input.
 * It is hidden there, and the way out is PWR or the app's own back button. */
void launcher_handle_show(bool on)
{
    if (!s_handle || !s_handle_hit) return;
    /* If the screen was deleted this pointer is a dead address. Ask LVGL before using it. */
    if (!lv_obj_is_valid(s_handle) || !lv_obj_is_valid(s_handle_hit)) {
        s_handle = NULL; s_handle_hit = NULL;
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_handle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_handle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Keep the picture, take away the grab pad.
 * 🚨 In apps where the whole screen is input (the trackpad), the handle's pad
 * must not take that area — a drag starting near the bottom would die
 * entirely. Those apps switch this on and decide for themselves using the
 * three functions below. */
void launcher_handle_passthrough(bool on)
{
    if (!s_handle_hit || !lv_obj_is_valid(s_handle_hit)) return;
    if (on) lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_remove_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
}

bool launcher_handle_zone(int32_t x, int32_t y)
{
    return y >= 466 - HANDLE_H
        && x >= (466 - HANDLE_W) / 2
        && x <  (466 + HANDLE_W) / 2;
}

/* Drag the picture with the finger, and report whether it went far enough to go home. */
bool launcher_handle_drag(int32_t dy)
{
    if (!s_handle || !lv_obj_is_valid(s_handle)) return false;
    if (dy > 0) dy = 0;
    if (dy < -60) dy = -60;
    lv_obj_set_style_translate_y(s_handle, dy, 0);
    lv_obj_set_style_arc_opa(s_handle, LV_OPA_COVER, LV_PART_MAIN);
    return dy <= -SWIPE_UP;
}

void launcher_handle_drop(void)
{
    if (!s_handle || !lv_obj_is_valid(s_handle)) return;
    lv_obj_set_style_translate_y(s_handle, 0, 0);
    lv_obj_set_style_arc_opa(s_handle, 150, LV_PART_MAIN);
}

/* 🚨 Position decided once, here. At dy -186 the circle is 140 px half-wide,
 * so dx -108 puts the outer end at 130 px — not clipped. Any higher and it is. */
static void back_btn_cb(lv_event_t *e)
{
    void (*act)(void) = (void (*)(void))lv_event_get_user_data(e);
    if (act) act();
}

lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void))
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 60, 40);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, -108, -186);
    lv_obj_add_event_cb(b, back_btn_cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD2D8E4), 0);
    lv_obj_center(l);
    return b;
}

void launcher_handle_add(lv_obj_t *parent, void (*action)(void))
{
    s_handle_action = action;   /* kept, though the callback no longer reads it */
    /* A straight bar does not suit a round body. It is drawn as a short arc
     * following the edge. LVGL angles start at 3 o'clock going clockwise, so
     * 90 degrees is the bottom. */
    s_handle = lv_arc_create(parent);
    lv_obj_set_size(s_handle, 438, 438);
    lv_obj_center(s_handle);
    lv_arc_set_bg_angles(s_handle, 82, 98);
    lv_arc_set_value(s_handle, 0);
    lv_obj_remove_style(s_handle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_handle, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_handle, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_handle, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_handle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_handle, 150, LV_PART_MAIN);
    /* Attach what this particular handle should do to the handle itself */
    lv_obj_set_user_data(s_handle, (void *)action);

    /* The arc is too thin to grab, so an invisible pad underneath takes the touch. */
    s_handle_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(s_handle_hit);
    lv_obj_set_size(s_handle_hit, HANDLE_W, HANDLE_H);
    lv_obj_align(s_handle_hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_CLICKABLE);
    /* Hand it its own arc — reading the static mixes in another screen's */
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_PRESSED,  s_handle);
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_PRESSING, s_handle);
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_RELEASED, s_handle);
}

/* LVGL deletes the old screen itself once the animation finishes (auto_del).
 * Deleting it ourselves mid-flight leaves animations still referencing it,
 * walking into freed memory — which killed the simulator outright. */
static void close_done_cb(void *a)
{
    (void)a;
    s_closing = false;
    port_heap_report("home");
    lock_stop();
    s_app_scr = NULL;
    s_stage = NULL;
    s_handle = NULL;
    s_handle_hit = NULL;
    s_current = NULL;
    radio_apply(RADIO_OFF);
}

void launcher_open(const badge_app_t *app)
{
    if (s_closing) return;   /* ignore while closing; home is about to appear */
    if (s_app_scr) return;              /* already inside an app */
    port_log(TAG, "open %s", app->name);

    radio_apply(app->radio);

    s_app_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_app_scr);
    lv_obj_set_style_bg_color(s_app_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_app_scr, LV_OPA_COVER, 0);

    /* App contents go on this object */
    s_stage = lv_obj_create(s_app_scr);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, 466, 466);
    lv_obj_center(s_stage);
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(s_stage, 233, 0);
    lv_obj_set_style_transform_pivot_y(s_stage, 233, 0);

    s_current = app;
    app->enter(s_stage);
    port_heap_report(app->name);
    launcher_handle_add(s_app_scr, launcher_home);   /* on top of the app's contents */

    /* A scale animation rescales and redraws the entire screen every frame,
     * which is visibly sluggish on an ESP32. Use LVGL's own screen fade. */
    lv_screen_load_anim(s_app_scr, LV_SCR_LOAD_ANIM_FADE_IN, ANIM_MS_OPEN, 0, false);
}

void launcher_home(void)
{
    /* With the display off, wake it instead of going home. An app closing on
     * an invisible screen leaves you with no idea what happened. */
    if (s_veil) { screen_wake(); return; }
    if (!s_app_scr || !s_stage || s_closing) return;
    port_crumb(CRUMB_HOME);
    port_log(TAG, "home");

    /* Tear the app down first — it must not shrink away with timers running */
    if (s_current && s_current->leave) s_current->leave();
    /* 🚨 Any wake lock the app took is released here, without exception. The
     * mouse app took one on every report and never released it on the way
     * out, so home stayed lit for minutes after leaving it. Letting each app
     * release its own means one missed case does exactly this — collect them
     * all at the door.
     * 🚨 AWAKE_STOP and AWAKE_RING are left alone. Those belong to things
     * that live outside apps (a running stopwatch, a ringing alarm). */
    launcher_keep_awake_by(AWAKE_APP, false);
    s_closing = true;

    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, ANIM_MS_CLOSE, 0, true);
    lv_timer_t *t = lv_timer_create((lv_timer_cb_t)close_done_cb, ANIM_MS_CLOSE + 30, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void launcher_start(void)
{
    build_home();
    /* Power-on goes to the lock screen (the clock). Home needs the handle pulled. */
    lv_obj_t *lk = lock_screen();
    lock_start();
    lv_screen_load_anim(lk, LV_SCR_LOAD_ANIM_FADE_IN, 260, 0, true);
    port_home_button_start(launcher_home);
    s_idle = lv_timer_create(idle_cb, 1000, NULL);
    s_heap_timer   = lv_timer_create(heap_cb, 10000, NULL);
    s_rotate_timer = lv_timer_create(rotate_poll, 200, NULL);
    if (!s_autorotate) lv_timer_pause(s_rotate_timer);   /* off by default */
    lv_timer_create(batt_log_cb, 60000, NULL);   /* once a minute; the journal writes every five */
    /* Once 30 s after boot, then every 30 minutes. Coming home puts you inside that window. */
    lv_timer_t *up = lv_timer_create(housekeep_cb, 30000, NULL);
    lv_timer_set_repeat_count(up, 1);
    lv_timer_create(housekeep_cb, 30 * 60 * 1000, NULL);

}
