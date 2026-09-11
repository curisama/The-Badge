/* WiFi, set up on the badge itself.
 *
 * 🚨 A device that needs a phone or a PC to be configured is not much of a
 * badge. So this does not do the usual thing of opening an access point and
 * making you find it in a browser. It scans, you pick, and you type the
 * password here (multi-tap keypad, keypad.c).
 *
 * 🚨 The first version showed the saved slots first — pick a slot, scan, pick
 * a network. That was wrong: slots are bookkeeping, not something anyone
 * wants to look at. Now the scan list is the screen:
 *
 *     tap a known network  -> joins with the saved password
 *     tap a new one        -> keypad, then save and join
 *     hold                 -> forget it
 *
 * Saving picks a free slot by itself.
 *
 * 🚨 The badge does not stay on WiFi. It joins to set the clock and leaves
 * again, because the radio costs more than it is worth. So green does not
 * mean "connected right now" — it means "this is the last network that
 * worked", which is the question you actually have ("is this one good?").
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr, *s_list, *s_note;
static lv_timer_t *s_poll;
static char        s_pick[33];             /* the SSID being joined */
static char        s_lastok[33];           /* last network that worked */
static wifi_found_t s_found[WIFI_SCAN_MAX];
static int          s_n;

static void show_scan(void);
static void rescan_cb(lv_timer_t *t) { (void)t; show_scan(); }

/* ── shared ───────────────────────────────────────────────── */
static void clear_body(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; }
    if (s_note) { lv_obj_delete(s_note); s_note = NULL; }
}

static void note(const char *txt, uint32_t col)
{
    if (!s_note) {
        s_note = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_note, &lv_font_montserrat_18, 0);
        lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_set_style_text_color(s_note, lv_color_hex(col), 0);
    lv_label_set_text(s_note, txt);
}

/* ── while joining ────────────────────────────────────────── */
static void try_poll(lv_timer_t *t)
{
    (void)t;
    int st = port_wifi_try_state();
    if (st == WIFI_TRY_BUSY) return;
    lv_timer_delete(s_poll); s_poll = NULL;

    if (st == WIFI_TRY_OK) {
        note("connected", 0x5BD48A);
    } else {
        /* 🚨 There is no clean way to tell a wrong password from a network
         * that has gone away, and both happen — so say both. Naming only one
         * sends people off fixing the wrong thing. */
        note("failed - wrong key?", 0xE06A6A);
    }
    /* Hold the result a moment, then back to the list so you see which row went green. */
    s_poll = lv_timer_create(rescan_cb, 1600, NULL);
    lv_timer_set_repeat_count(s_poll, 1);
}

static void try_now(const char *ssid, const char *pass)
{
    clear_body();
    note("connecting...", 0xE0B33A);
    port_wifi_try(ssid, pass);
    s_poll = lv_timer_create(try_poll, 300, NULL);
}

/* Password entered — save it and try it straight away. */
static void pass_done(const char *text)
{
    if (!text) { show_scan(); return; }        /* cancelled */
    int slot = port_wifi_slot_find(s_pick);
    if (slot < 0) slot = port_wifi_slot_free();
    port_wifi_slot_set(slot, s_pick, text);
    try_now(s_pick, text);
}

/* ── the list ─────────────────────────────────────────────── */
static void ap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    snprintf(s_pick, sizeof s_pick, "%s", s_found[i].ssid);
    if (s_found[i].saved) {
        /* Already known — nothing to type. Just join. */
        try_now(s_pick, NULL);
    } else {
        clear_body();
        keypad_open(s_pick, NULL, pass_done);
    }
}

/* Hold to forget */
static void forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n || !s_found[i].saved) return;
    port_wifi_slot_clear(s_found[i].saved - 1);
    show_scan();
}

static void scan_poll(lv_timer_t *t)
{
    (void)t;
    int n = port_wifi_scan_result(s_found, WIFI_SCAN_MAX);
    if (n < 0) return;                          /* still scanning */
    s_n = n;
    lv_timer_delete(s_poll); s_poll = NULL;
    if (s_note) { lv_obj_delete(s_note); s_note = NULL; }
    if (s_n == 0) { note("nothing around", 0x6E7686); return; }

    /* 🚨 Saved networks float to the top. The one you always use being ten
     * rows down means scrolling to find it, and that is most of what "tap it
     * and it joins" was worth.
     * Within each group the scan order is kept, which is already strongest
     * first. The sort is stable, so rows do not jump around. */
    for (int i = 1; i < s_n; i++) {
        wifi_found_t k = s_found[i];
        if (!k.saved) continue;
        int j = i - 1;
        while (j >= 0 && !s_found[j].saved) { s_found[j + 1] = s_found[j]; j--; }
        s_found[j + 1] = k;
    }

    port_wifi_last_ok(s_lastok, sizeof s_lastok);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 300);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_n; i++) {
        bool ok = s_lastok[0] && strcmp(s_lastok, s_found[i].ssid) == 0;
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 62);
        lv_obj_set_style_radius(b, 16, 0);
    /* The network that last worked is green outright; merely saved is dimmer. */
        lv_obj_set_style_bg_color(b, lv_color_hex(
            ok ? 0x2E6E5A : (s_found[i].saved ? 0x24343A : 0x1D1D24)), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_left(b, 18, 0);
        lv_obj_add_event_cb(b, ap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (s_found[i].saved)
            lv_obj_add_event_cb(b, forget_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *t2 = lv_label_create(b);
        lv_label_set_text(t2, s_found[i].ssid);
        lv_obj_set_style_text_font(t2, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t2, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(t2, LV_ALIGN_LEFT_MID, 0, -11);

        char sub[44];
        if (ok)                    snprintf(sub, sizeof sub, "%d dBm  -  connected", s_found[i].rssi);
        else if (s_found[i].saved) snprintf(sub, sizeof sub, "%d dBm  -  saved", s_found[i].rssi);
        else                       snprintf(sub, sizeof sub, "%d dBm", s_found[i].rssi);
        lv_obj_t *s2 = lv_label_create(b);
        lv_label_set_text(s2, sub);
        lv_obj_set_style_text_font(s2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s2, lv_color_hex(ok ? 0xCDEEDD : 0x8A93A6), 0);
        lv_obj_align(s2, LV_ALIGN_LEFT_MID, 0, 12);
    }
}

static void show_scan(void)
{
    clear_body();
    note("looking around...", 0x6E7686);
    port_wifi_scan_start();
    s_poll = lv_timer_create(scan_poll, 200, NULL);
}

/* ── open and close ───────────────────────────────────────── */
static void wifi_setup_close(void)
{
    clear_body();
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
}

void wifi_setup_open(void)
{
    if (s_scr) return;
    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(s_scr);
    lv_label_set_text(t, "Wi-Fi");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -186);

    lv_obj_t *h = lv_label_create(s_scr);
    lv_label_set_text(h, "tap to join  -  hold to forget");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 186);

    /* The handle gets you out, and so does the back button.
     * 🚨 With only the handle, only people who already know it can leave. */
    ui_back_btn(s_scr, wifi_setup_close);
    launcher_handle_add(s_scr, wifi_setup_close);
    show_scan();               /* scan as soon as it opens */
}
