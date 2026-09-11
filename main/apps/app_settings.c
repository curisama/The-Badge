/* Settings.
 *
 * 🚨 This used to be six pages swiped through vertically. As items were added
 * the last one (WiFi) ended up five swipes deep. **Now the list comes first
 * and only the chosen page opens.** Everything visible at once, with the
 * values written in the list so that nobody opens a page just to read one —
 * which is why the battery is a row rather than a page.
 *
 * The controls are still the ring around the edge: it is the largest thing on
 * the screen and a control only a round display can offer.
 *
 * 🚨 The detail pages live on the top layer (lv_layer_top). They cover the
 * list without touching it, and lifting them returns to it exactly — the list
 * never has to be rebuilt.
 *
 * 🚨 The screen-margin (Edge) page was removed. It is set once and never
 * again, and it only took up space. The setting itself remains (display.c
 * uses it) — if it ever needs changing, change the value in code rather than
 * bringing the page back. */
#include "app.h"
#include "port.h"
#include "display.h"
#include <stdio.h>

static lv_obj_t *s_gap_lbl;

static bool s_sound_on = true;
static int  s_volume   = 60;

/* ── storage ─────────────────────────────────────────────────── */

typedef struct {
    int32_t bright, volume, timeout, sound_on, xgap;
} settings_t;

static void settings_save(void)
{
    settings_t v = { port_brightness_get(), s_volume, launcher_get_timeout(), s_sound_on,
                     badge_display_get_xgap() };
    port_kv_write("cfg", &v, sizeof(v));
}

static void gap_paint(void)
{
    if (s_gap_lbl) lv_label_set_text_fmt(s_gap_lbl, "x gap %d", badge_display_get_xgap());
}

/* Cycles through the candidates. The right value is 8 for a 480-column
 * controller, 6 for 478 (unchanged), 4 for 476, 0 for 472. Pick whichever
 * makes the coloured band at the edge disappear. */
static void gap_cb(lv_event_t *e)
{
    (void)e;
    static const int CAND[] = { 6, 8, 4, 0, 2, 10, 12 };
    const int N = (int)(sizeof(CAND) / sizeof(CAND[0]));
    int cur = badge_display_get_xgap(), i = 0;
    for (int k = 0; k < N; k++) if (CAND[k] == cur) { i = k; break; }
    badge_display_set_xgap(CAND[(i + 1) % N]);
    gap_paint();
    settings_save();
}

void settings_load(void)
{
    settings_t v;
    if (!port_kv_read("cfg", &v, sizeof(v))) {
        /* With no stored setting, the BSP's default of 100% remains. On a
         * 1.75-inch AMOLED that is dazzling indoors, and current scales
         * directly with brightness. Start at 45% instead. */
        port_brightness_set(45);
        return;
    }
    if (v.bright >= 5 && v.bright <= 100) port_brightness_set(v.bright);
    if (v.volume >= 0 && v.volume <= 100) s_volume = v.volume;
    s_sound_on = v.sound_on != 0;
    launcher_set_timeout(v.timeout);
    port_tone_volume(s_sound_on ? s_volume : 0);
    if (v.xgap >= 0 && v.xgap <= 16) badge_display_set_xgap(v.xgap);
}

/* ── shared pieces ───────────────────────────────────────────── */

static lv_obj_t *title(lv_obj_t *p, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -140);
    return l;
}

static lv_obj_t *big(lv_obj_t *p, int dy)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

/* The thick ring around the edge — the easiest thing on this screen to grab. */
static lv_obj_t *ring(lv_obj_t *p, int lo, int hi, int val, lv_event_cb_t cb, uint32_t col)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_set_size(a, 404, 404);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, lo, hi);
    lv_arc_set_value(a, val);
    lv_obj_set_style_arc_width(a, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 26, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x1E1E24), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(a, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 12, LV_PART_KNOB);
    /* Without this the ring's whole bounding box becomes clickable, and a
     * swipe across the middle of the screen is taken by the ring instead of
     * turning the page. */
    lv_obj_add_flag(a, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(a, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return a;
}

/* ── page: brightness ───────────────────────────────────────── */

static lv_obj_t *s_bright_lbl;

static void bright_cb(lv_event_t *e)
{
    int v = lv_arc_get_value(lv_event_get_target(e));
    port_brightness_set(v);
    lv_label_set_text_fmt(s_bright_lbl, "%d%%", v);
    settings_save();
}

/* ── page: sound ────────────────────────────────────────────── */

static lv_obj_t *s_vol_lbl, *s_sound_btn_lbl;

static void vol_cb(lv_event_t *e)
{
    s_volume = lv_arc_get_value(lv_event_get_target(e));
    if (s_sound_on) port_tone_volume(s_volume);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_volume);
    settings_save();
}

static void sound_cb(lv_event_t *e)
{
    (void)e;
    s_sound_on = !s_sound_on;
    port_tone_volume(s_sound_on ? s_volume : 0);
    lv_label_set_text(s_sound_btn_lbl, s_sound_on ? "ON" : "OFF");
    settings_save();
}

/* ── page: display timeout ──────────────────────────────────── */

static const int TIMEOUTS[4] = { 15, 30, 60, 0 };
static lv_obj_t *s_to_btn[4];

static lv_obj_t *s_rot_lbl;

static void rot_cb(lv_event_t *e)
{
    (void)e;
    bool on = !launcher_get_autorotate();
    launcher_set_autorotate(on);
    lv_label_set_text(s_rot_lbl, on ? "auto-rotate ON" : "auto-rotate OFF");
}

static void timeout_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    launcher_set_timeout(TIMEOUTS[i]);
    for (int k = 0; k < 4; k++)
        lv_obj_set_style_bg_color(s_to_btn[k], lv_color_hex(k == i ? 0x2E6E9E : 0x1C1C22), 0);
    settings_save();
}

/* ── page: clock ────────────────────────────────────────────── */

typedef struct { int min; const char *name; } tz_t;
static const tz_t TZS[] = {
    { -600, "Honolulu" }, { -480, "Los Angeles" }, { -420, "Denver" }, { -360, "Chicago" },
    { -300, "New York" }, { -180, "Sao Paulo" }, { 0, "London" }, { 60, "Paris" },
    { 120, "Athens" }, { 180, "Moscow" }, { 210, "Tehran" }, { 240, "Dubai" },
    { 270, "Kabul" }, { 300, "Karachi" }, { 330, "Delhi" }, { 345, "Kathmandu" },
    { 360, "Dhaka" }, { 390, "Yangon" }, { 420, "Bangkok" }, { 480, "Shanghai" },
    { 525, "Eucla" }, { 540, "Seoul" }, { 570, "Adelaide" }, { 600, "Sydney" },
    { 720, "Auckland" }, { 780, "UTC+13" }, { 840, "UTC+14" },
};
#define TZ_CNT (sizeof(TZS) / sizeof(TZS[0]))

static lv_obj_t   *s_net_lbl;
static lv_timer_t *s_poll;

static void tz_cb(lv_event_t *e)
{
    uint32_t i = lv_roller_get_selected(lv_event_get_target(e));
    if (i < TZ_CNT) port_set_tz_offset(TZS[i].min);
}

static void sync_cb(lv_event_t *e) { (void)e; port_time_sync_start(); }
/* 🚨 WiFi setup opens **outside** this app's tileview. Inside it, scrolling
 * the list and turning tiles fight each other. */
static void wifi_cb(lv_event_t *e) { (void)e; wifi_setup_open(); }

static void list_paint(void);

/* 🚨 This function must only touch what is on screen right now. Removing the
 * battery page deleted s_bat_* while this still used them — writing text into
 * a NULL label stopped it dead. The labels only exist while their page is
 * open. Showing values is the list's job (list_paint). */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    list_paint();                       /* battery and connection state live in the list */

    if (!s_net_lbl || !lv_obj_is_valid(s_net_lbl)) return;   /* the clock page is closed */
    static const char *TXT[] = { "not set", "idle", "connecting", "synced", "failed" };
    static const uint32_t COL[] = { 0x666666, 0x8A8A8A, 0xE0B33A, 0x5BD48A, 0xE06A6A };
    net_state_t st = port_time_sync_state();
    lv_label_set_text(s_net_lbl, TXT[st]);
    lv_obj_set_style_text_color(s_net_lbl, lv_color_hex(COL[st]), 0);
}


/* ── the app ────────────────────────────────────────────────── */

static lv_obj_t *pill(lv_obj_t *p, const char *txt, int w, int h, int dx, int dy,
                      uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

/* ── the detail pages ──────────────────────────────────────────
 * Choosing from the list brings one up on top. Pulling the handle lifts it. */
static lv_obj_t *s_page;

static void page_close(void)
{
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    list_paint();                 /* so a changed value shows in the list immediately */
}

static lv_obj_t *page_open(const char *name)
{
    if (s_page) return NULL;
    s_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, 466, 466);
    lv_obj_center(s_page);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_CLICKABLE);   /* stop touches falling through */
    title(s_page, name);
    ui_back_btn(s_page, page_close);
    launcher_handle_add(s_page, page_close);
    return s_page;
}

/* ── the pages ─────────────────────────────────────────────── */
static void open_bright(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Brightness");
    if (!p) return;
    s_bright_lbl = big(p, 0);
    int b = port_brightness_get();
    lv_label_set_text_fmt(s_bright_lbl, "%d%%", b);
    ring(p, 5, 100, b, bright_cb, 0x7FB0FF);
}

static void open_sound(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Sound");
    if (!p) return;
    s_vol_lbl = big(p, -26);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_volume);
    ring(p, 0, 100, s_volume, vol_cb, 0x5BD48A);
    lv_obj_t *sb = pill(p, s_sound_on ? "ON" : "OFF", 140, 60, 0, 66,
                        0x1C1C22, sound_cb, NULL);
    s_sound_btn_lbl = lv_obj_get_child(sb, 0);
}

static void open_screen(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Screen off");
    if (!p) return;
    const char *TO_TXT[4] = { "15s", "30s", "60s", "never" };
    int cur = launcher_get_timeout();
    for (int i = 0; i < 4; i++) {
        s_to_btn[i] = pill(p, TO_TXT[i], 150, 68,
                           (i % 2) ? 82 : -82, (i / 2) ? 48 : -32,
                           TIMEOUTS[i] == cur ? 0x2E6E9E : 0x1C1C22,
                           timeout_cb, (void *)(intptr_t)i);
    }
    lv_obj_t *rb = pill(p, launcher_get_autorotate() ? "auto-rotate ON" : "auto-rotate OFF",
                        280, 54, 0, 136, 0x1C1C22, rot_cb, NULL);
    s_rot_lbl = lv_obj_get_child(rb, 0);
}

static void open_time(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Time");
    if (!p) return;
    lv_obj_t *rl = lv_roller_create(p);
    char opts[TZ_CNT * 14];
    int n = 0, sel = 0, cur_tz = port_get_tz_offset();
    for (unsigned i = 0; i < TZ_CNT; i++) {
        n += snprintf(opts + n, sizeof(opts) - n, "%s%s", i ? "\n" : "", TZS[i].name);
        if (TZS[i].min == cur_tz) sel = i;
    }
    lv_roller_set_options(rl, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(rl, 3);
    lv_obj_set_width(rl, 300);
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_bg_color(rl, lv_color_hex(0x141418), 0);
    lv_obj_set_style_border_width(rl, 0, 0);
    lv_obj_set_style_radius(rl, 22, 0);
    lv_obj_set_style_bg_color(rl, lv_color_hex(0x2E4A66), LV_PART_SELECTED);
    lv_roller_set_selected(rl, sel, LV_ANIM_OFF);
    lv_obj_add_event_cb(rl, tz_cb, LV_EVENT_VALUE_CHANGED, NULL);

    pill(p, "sync now", 220, 62, 0, 74, 0x1C1C22, sync_cb, NULL);
    s_net_lbl = lv_label_create(p);
    lv_obj_set_style_text_font(s_net_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_net_lbl, LV_ALIGN_CENTER, 0, 136);
    poll_cb(NULL);
}

static void open_hosts(lv_event_t *e)
{
    /* 🚨 The host list opens its own screen — page_open here would stack two. */
    (void)e;
    host_pick_open();
}

static void open_wifi(lv_event_t *e)
{
    /* 🚨 WiFi opens its own screen (wifi_setup.c). page_open here would stack
     * two and tangle the handle. */
    (void)e;
    wifi_setup_open();
}

/* ── the list ──────────────────────────────────────────────── */
static lv_obj_t *s_list;
static lv_obj_t *s_sub[7];          /* the value text on each row */

static lv_obj_t *menu_row(lv_obj_t *parent, const char *name, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 340, 62);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(cb ? 0x1D1D24 : 0x141418), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 18, 0);
    lv_obj_set_style_pad_right(b, 18, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    else    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, name);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(cb ? 0xE8ECF0 : 0x9AA4AE), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(b);
    lv_label_set_text(v, "");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;                        /* hand back the value label */
}

/* 🚨 Writing values into the list is the point — so nobody opens a page to read one. */
static void list_paint(void)
{
    if (!s_sub[0]) return;
    lv_label_set_text_fmt(s_sub[0], "%d%%", port_brightness_get());
    lv_label_set_text_fmt(s_sub[1], "%d%%  %s", s_volume, s_sound_on ? "on" : "off");

    int to = launcher_get_timeout();
    if (to <= 0) lv_label_set_text(s_sub[2], "never");
    else         lv_label_set_text_fmt(s_sub[2], "%ds", to);

    static const char *NET[] = { "not set", "idle", "connecting", "synced", "failed" };
    int tz = port_get_tz_offset();
    const char *tzn = "";
    for (unsigned i = 0; i < TZ_CNT; i++) if (TZS[i].min == tz) tzn = TZS[i].name;
    lv_label_set_text_fmt(s_sub[3], "%s  %s", tzn, NET[port_time_sync_state()]);

    int saved = 0;
    for (int i = 0; i < WIFI_SLOTS; i++) {
        char ss[33];
        if (port_wifi_slot_get(i, ss, sizeof ss)) saved++;
    }
    lv_label_set_text_fmt(s_sub[4], "%d saved", saved);

    hid_host_t hh[HID_HOSTS_MAX];
    int nh = port_hid_hosts(hh, HID_HOSTS_MAX);
    lv_label_set_text_fmt(s_sub[5], "%d paired", nh);

    /* The battery gets no page: there is no reason to press again just to see a number. */
    int p = port_battery_percent();
    if (p < 0) {
        lv_label_set_text(s_sub[6], "--");
    } else if (port_battery_plugged()) {
        lv_label_set_text_fmt(s_sub[6], "%d%%  charging", p);
    } else {
        int m = port_battery_minutes_left();
        if (m < 0) lv_label_set_text_fmt(s_sub[6], "%d%%  measuring", p);
        else       lv_label_set_text_fmt(s_sub[6], "%d%%  %dh %02dm", p, m / 60, m % 60);
    }
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_SETTINGS);
    s_page = NULL;
    s_net_lbl = NULL;

    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, "Settings");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -190);

    /* 🚨 On a round screen the list uses the wide middle and scrolls
     * vertically. Rows are a generous 62 px so that scrolling does not
     * accidentally press one. */
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 330);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_sub[0] = menu_row(s_list, "Brightness", open_bright);
    s_sub[1] = menu_row(s_list, "Sound",      open_sound);
    s_sub[2] = menu_row(s_list, "Screen off", open_screen);
    s_sub[3] = menu_row(s_list, "Time",       open_time);
    s_sub[4] = menu_row(s_list, "Wi-Fi",      open_wifi);
    s_sub[5] = menu_row(s_list, "Bluetooth",  open_hosts);
    s_sub[6] = menu_row(s_list, "Battery",    NULL);   /* information only */

    list_paint();
    s_poll = lv_timer_create(poll_cb, 5000, NULL);   /* the battery moves a step every few minutes */
}

static void leave(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    /* 🚨 Detail pages are on the top layer and are not deleted with the app's
     * screen. Left up, a settings page covers the display even after going home. */
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_list = NULL;
    for (int i = 0; i < 7; i++) s_sub[i] = NULL;
    s_net_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x9AA0A6); }

const badge_app_t app_settings = {
    .name = "Settings", .icon = LV_SYMBOL_SETTINGS, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
