/* Pick which host to be connected to.
 *
 * 🚨 A BLE bond stores the address and nothing else. The host's name never
 * arrives — the badge is the peripheral, so it has no reason to see one. A
 * list built from bonds alone is three rows of `A4:83:E7:11:22:33` with no
 * way to tell which one is the PC at home. This screen only became worth
 * having once there was a keypad to name them with.
 *
 * 🚨 The awkward part is the old host racing to reconnect. A whitelist alone
 * leaks depending on the stack, so hid_mouse.c has a second guard that checks
 * the address on connect and drops it if it is the wrong one. */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr, *s_list;
static lv_timer_t *s_poll;
static hid_host_t  s_hosts[HID_HOSTS_MAX];
static int         s_n;
static int         s_editing = -1;
static bool        s_built;            /* does s_hosts match what is drawn? */

static void show_list(void);

/* 🚨 **Rebuilding the screen under a finger kills that press.**
 *
 * When the object being pressed is deleted, LVGL calls
 * `lv_indev_wait_release()` and ignores that input device until the finger
 * lifts (lv_obj_tree.c). A new object in the same place gets no PRESSED, so a
 * long press never completes — pressing harder or longer does nothing.
 * Reported as "holding it doesn't bring up the keyboard".
 *
 * This was the only screen rebuilding its whole list every two seconds. Two
 * guards now:
 *   1. if nothing changed, do not rebuild (normally: never)
 *   2. if something did change but a finger is down, wait */
static bool touching(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

static bool same_as_drawn(const hid_host_t *a, int n)
{
    if (!s_built || n != s_n) return false;
    for (int i = 0; i < n; i++) {
        if (memcmp(a[i].addr, s_hosts[i].addr, 6) != 0) return false;
        if (a[i].here != s_hosts[i].here) return false;
        if (strcmp(a[i].name, s_hosts[i].name) != 0) return false;
    }
    return true;
}

static void relist_cb(lv_timer_t *t)
{
    (void)t;
    hid_host_t now[HID_HOSTS_MAX];
    int n = port_hid_hosts(now, HID_HOSTS_MAX);
    if (same_as_drawn(now, n)) return;
    if (touching()) return;            /* look again next time round */
    show_list();
}

static void addr_txt(const uint8_t a[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Name entered */
static void name_done(const char *text)
{
    if (text && s_editing >= 0 && s_editing < s_n)
        port_hid_host_name_set(s_hosts[s_editing].addr, text);
    s_editing = -1;
    show_list();
}

/* Tap = switch to that host */
static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    port_hid_host_pick(s_hosts[i].addr);
    show_list();
}

/* Renaming, by holding the row or by the pencil at the end of it.
 * 🚨 Hold-only means only people who already know about it can rename
 * anything — the same reason the back button exists. */
static void rename_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    s_editing = i;
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; s_built = false; }
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    keypad_open("name this host", s_hosts[i].name, name_done);
}

static void any_cb(lv_event_t *e)
{
    (void)e;
    port_hid_host_any();
    show_list();
}

static void show_list(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; }
    s_n = port_hid_hosts(s_hosts, HID_HOSTS_MAX);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 300);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_n; i++) {
        char sub[24];
        addr_txt(s_hosts[i].addr, sub, sizeof sub);
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 62);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(s_hosts[i].here ? 0x24343A : 0x1D1D24), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_left(b, 18, 0);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, rename_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        /* The pencil sits inside the row. Touches here go to it, not the row. */
        lv_obj_t *ed = lv_button_create(b);
        lv_obj_set_size(ed, 54, 46);
        lv_obj_set_style_radius(ed, 14, 0);
        lv_obj_set_style_bg_color(ed, lv_color_hex(0x2A2A34), 0);
        lv_obj_set_style_shadow_width(ed, 0, 0);
        lv_obj_align(ed, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(ed, rename_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *el = lv_label_create(ed);
        lv_label_set_text(el, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(el, lv_color_hex(0xB6C2D6), 0);
        lv_obj_center(el);

        lv_obj_t *t = lv_label_create(b);
        /* 🚨 Unnamed hosts show their address. A blank row cannot be chosen. */
        lv_label_set_text(t, s_hosts[i].name[0] ? s_hosts[i].name : sub);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -11);

        lv_obj_t *s = lv_label_create(b);
        lv_label_set_text(s, s_hosts[i].here ? "connected" : sub);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(s_hosts[i].here ? 0x5BD48A : 0x8A93A6), 0);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 12);
    }

    if (s_n == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_label_set_text(e, "no paired host yet");
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(0x6E7686), 0);
    } else {
        /* Release the pinned host so anything can pair — needed for a new device. */
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 54);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141418), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, any_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, "accept any");
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0x9AA4AE), 0);
        lv_obj_center(t);
    }

    s_built = true;

    /* Connecting takes a few seconds, and the list should follow along.
     * 🚨 lv_timer_cb_t takes an argument. Casting show_list to it is a
     * mismatched call that breaks on some platforms — wrap it instead.
     * 🚨 This only rebuilds when something actually changed (see relist_cb). */
    s_poll = lv_timer_create(relist_cb, 2000, NULL);
}

static void host_pick_close(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_scr)  { lv_obj_delete(s_scr); s_scr = NULL; }
    s_list = NULL;
    s_built = false;
}

void host_pick_open(void)
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
    lv_label_set_text(t, "Hosts");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -186);

    lv_obj_t *h = lv_label_create(s_scr);
    lv_label_set_text(h, "tap to switch  -  " LV_SYMBOL_EDIT " to name");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 186);

    ui_back_btn(s_scr, host_pick_close);
    launcher_handle_add(s_scr, host_pick_close);
    show_list();
}
