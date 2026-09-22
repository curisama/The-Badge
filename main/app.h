#pragma once
#include "lvgl.h"

/* What radio an app needs. WiFi and BLE share one antenna, so having both on
 * makes both slow — the launcher sorts it out when apps change. */
typedef enum {
    RADIO_OFF = 0,   /* games and anything offline */
    RADIO_BLE,       /* air mouse (BLE HID)        */
    RADIO_WIFI,      /* brief, like setting the clock */
} radio_need_t;

typedef struct {
    const char  *name;
    const lv_image_dsc_t *art;  /* home tile picture; falls back to `icon` */
    const char  *icon;          /* LVGL symbol */
    lv_color_t (*tint)(void);   /* home icon colour */
    radio_need_t radio;
    bool         keep_awake;    /* true skips the display timeout */
    void (*enter)(lv_obj_t *root);  /* build the screen  */
    void (*leave)(void);            /* free timers and the rest */
} badge_app_t;

extern const badge_app_t app_mouse;
extern const badge_app_t app_clock;
extern const badge_app_t app_settings;
extern const badge_app_t app_keys;
extern const badge_app_t app_calc;
extern const badge_app_t app_games;
extern const badge_app_t app_meet;
extern const badge_app_t app_air;   /* the mouse app, entered in air mode */
extern const badge_app_t app_water; /* a board inside Games, opened from home */
extern const badge_app_t app_orb;   /* planets — pick one of five inside */

/* ── multi-tap keypad ─────────────────────────────────────────
 * Old phone style: press a key repeatedly to walk through its letters. It
 * floats on the top layer and leaves the calling app's screen alone — when it
 * closes the app is still there.
 * done(text) on finish, done(NULL) if the handle was pulled to cancel. */
void keypad_open(const char *title, const char *initial,
                 void (*done)(const char *text));
void keypad_close(void);
bool keypad_is_open(void);

/* WiFi setup, done on the badge itself. Also floats above the caller. */
void wifi_setup_open(void);

/* The screen that exports recordings as a USB drive (apps/usb_screen.c).
 * 🚨 Entering it makes the serial port disappear — see usb_msc.c. */
void usb_screen_open(void);
/* Hosts this badge has paired with: switch to one, or hold to rename it. */
void host_pick_open(void);

void launcher_start(void);
void launcher_screen_off(void);          /* timed out — touch wakes it  */
void launcher_screen_off_manual(void);   /* you turned it off — button only */
void launcher_screen_toggle(void);
void launcher_poweroff_notice(void);
void splash_show(void);
/* The lock screen. Waking the display lands here. */
lv_obj_t *lock_screen(void);
void      lock_start(void);
void      lock_stop(void);
void      launcher_show_home(void);
/* Unlock — back to the app that was open, or home if there wasn't one. */
void      launcher_unlock(void);
void      launcher_show_lock(void);
/* A back button, for screens that have somewhere to go back to.
 * 🚨 **Not on an app's first screen.** The way out of there is the handle,
 * and a second way out just raises the question of which one is right. This
 * is for screens one level deeper.
 * It always sits top left. An app that puts it somewhere else makes you hunt
 * for it every time. */
lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void));

void      launcher_handle_add(lv_obj_t *parent, void (*action)(void));
void      launcher_handle_show(bool on);   /* hidden during games */
/* Keep the handle drawn but stop it grabbing touches — for apps that do their
 * own hit testing. */
void      launcher_handle_passthrough(bool on);
bool      launcher_handle_zone(int32_t x, int32_t y);
bool      launcher_handle_drag(int32_t dy);   /* dragged, and pulled home? */
void      launcher_handle_drop(void);
void settings_load(void);
void launcher_keep_awake(bool on);
/* Several things can hold the screen awake at once. It sleeps when the last
 * one lets go. */
#define AWAKE_APP   0    /* a whole app, like the mouse  */
#define AWAKE_STOP  1    /* the stopwatch is running     */
#define AWAKE_RING  2    /* an alarm or timer is ringing */
void launcher_keep_awake_by(int who, bool on);

/* ── pages of the Clock app ─────────────────────────────────── */
void timer_build(lv_obj_t *root);
void timer_free(void);
void stopwatch_build(lv_obj_t *root);
void stopwatch_free(void);
void alarm_build(lv_obj_t *root);
void alarm_free(void);
/* 🚨 The alarm is the one that runs outside its app — the launcher ticks it. */
void alarm_tick(void);
bool alarm_armed(void);   /* is one set — the launcher asks before going dark */

/* 🚨 Never call lv_timer_set_period() from inside a timer callback.
 * It calls lv_timer_handler_resume(), so the handler restarts right there and
 * never comes back — 100%% CPU. Passing the period it already has does not
 * help, so "only set it when it changes" does not save you either.
 * To do less work while the display is off, leave the period alone and act on
 * every Nth call. Same effect, and it cannot bite. */
void launcher_screen_on(void);       /* alarms and such — wake it if asleep */
bool launcher_screen_is_off(void);   /* if it is off, poll lazily */
void launcher_set_autorotate(bool on);
bool launcher_get_autorotate(void);
void launcher_set_timeout(int seconds);   /* 0 = never */
int  launcher_get_timeout(void);
void launcher_home(void);
void launcher_open(const badge_app_t *app);
