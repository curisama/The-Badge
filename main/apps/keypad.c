/* Multi-tap keypad — old phone style: press a key repeatedly to walk through
 * the letters on it.
 *
 * 🚨 Why not QWERTY: 26 keys on a 466 px circle leaves each one 50x40 px, and
 * the rounded corners eat the keys at either end. Twelve keys gives 104x68,
 * which is three times the area.
 *
 * 🚨 Round does not mean "put it anywhere", though. A row of three keys is
 * about 340 px, and that only fits within 159 px of the centre vertically:
 *
 *     radius 233, half-width 170 needed  ->  sqrt(233^2 - 170^2) = 159
 *
 * Past that the circle is too narrow and the outer keys get clipped. So the
 * four rows are squeezed into dy -118..+98 and the keys came out 68 px tall.
 * The furthest corner sits 222 px from the centre.
 *
 * 🚨 A next-character key is not optional. With only a timeout you cannot
 * type "ab" — there is no way to take two letters off the same key in a row,
 * and WiFi passwords are full of exactly that.
 *
 * 🚨 What you have typed stays visible. Nobody types twenty characters of
 * multi-tap blind, and this is a device you hold in your hand, so the
 * shoulder-surfing risk is low.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

#define KP_MAX 64                /* WPA2 allows 63 characters, plus the NUL */
#define KP_TAP_MS 800            /* press the same key again within this to advance */

/* What is on each key: four modes x nine keys, plus key 0.
 * 🚨 Symbols get real room here. This is mostly for passwords, and letters
 * alone would make it half useless. */
static const char *SET[4][10] = {
    /* abc */ { " 0", ".,?!-", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" },
    /* ABC */ { " 0", ".,?!-", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" },
    /* 123 */ { "0",  "1", "2", "3", "4", "5", "6", "7", "8", "9" },
    /* sym */ { " 0", "@#$%", "&*()", "-_=+", "[]{}", "<>/\\", ":;'\"", "!?.,", "^~`|", "" },
};
static const char *MODE_NAME[4] = { "abc", "ABC", "123", "!@#" };

static char       s_buf[KP_MAX];
static int        s_len;
static uint8_t    s_mode;
static int        s_last_key = -1;      /* key just pressed (-1 = none) */
static int        s_tap;                /* how many times in a row        */
static uint32_t   s_tap_ms;
static lv_obj_t  *s_scr, *s_field, *s_modelbl;
static lv_obj_t  *s_keylbl[10];        /* key faces, repainted per mode */
static void      (*s_done)(const char *text);

/* 🚨 Fixed key faces hide the mode. Switch to symbols and every key still
 * reading "2 abc" means nobody finds the symbols at all. So all nine faces
 * are repainted whenever the mode changes. */
static void kp_face(int k, char *out, size_t cap)
{
    const char *set = SET[s_mode][k];
    if (s_mode == 2) {                      /* digits — just the number, large */
        snprintf(out, cap, "%s", set);
    } else if (s_mode == 3) {               /* symbols — show them as they are */
        snprintf(out, cap, "%s", set[0] ? set : "-");
    } else if (k == 0) {
        snprintf(out, cap, "0 _");          /* zero and space */
    } else {
        snprintf(out, cap, "%d %s", k, set); /* number plus its letters */
    }
}

static void kp_paint(void)
{
    if (!s_field) return;
    /* Right aligned: once it is long, what matters is the end you just typed. */
    lv_label_set_text(s_field, s_len ? s_buf : " ");
    if (s_modelbl) lv_label_set_text(s_modelbl, MODE_NAME[s_mode]);
    for (int k = 0; k < 10; k++) {
        if (!s_keylbl[k]) continue;
        char f[16];
        kp_face(k, f, sizeof f);
        lv_label_set_text(s_keylbl[k], f);
    }
}

/* Commit the pending character — on another key, on next, or on timeout. */
static void kp_commit(void)
{
    s_last_key = -1;
    s_tap = 0;
}

static void kp_key(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    const char *set = SET[s_mode][k];
    if (!set || !set[0]) return;
    int n = (int)strlen(set);
    uint32_t now = lv_tick_get();

    if (k == s_last_key && now - s_tap_ms < KP_TAP_MS && s_len > 0) {
        /* Same key again: swap the last character for the next one along */
        s_tap = (s_tap + 1) % n;
        s_buf[s_len - 1] = set[s_tap];
    } else {
        if (s_len >= KP_MAX - 1) return;
        s_tap = 0;
        s_buf[s_len++] = set[0];
        s_buf[s_len] = '\0';
        s_last_key = k;
    }
    s_tap_ms = now;
    kp_paint();
}

static void kp_next(lv_event_t *e)   { (void)e; kp_commit(); }

static void kp_mode(lv_event_t *e)
{
    (void)e;
    s_mode = (uint8_t)((s_mode + 1) % 4);
    kp_commit();
    kp_paint();
}

static void kp_back(lv_event_t *e)
{
    (void)e;
    if (s_len > 0) s_buf[--s_len] = '\0';
    kp_commit();
    kp_paint();
}

static void kp_ok(lv_event_t *e)
{
    (void)e;
    void (*cb)(const char *) = s_done;
    s_done = NULL;
    /* 🚨 Take the screen down before calling back. The caller usually opens
     * something else, and this would sit on top of it. */
    keypad_close();
    if (cb) cb(s_buf);
}

static void kp_cancel(void);
static void kp_cancel_btn(lv_event_t *e) { (void)e; kp_cancel(); }

/* Pulling the handle cancels, and so does the back button. */
static void kp_cancel(void)
{
    void (*cb)(const char *) = s_done;
    s_done = NULL;
    keypad_close();
    if (cb) cb(NULL);
}

static lv_obj_t *kp_btn(lv_obj_t *root, int dx, int dy, int w, int h,
                        const char *txt, lv_event_cb_t cb, int arg, uint32_t col)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x44444E), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD2D8E4), 0);
    lv_obj_center(l);
    return b;
}

void keypad_open(const char *title, const char *initial,
                 void (*done)(const char *text))
{
    s_done = done;
    s_mode = 0;
    s_last_key = -1;
    s_tap = 0;
    s_len = 0;
    s_buf[0] = '\0';
    if (initial) {
        snprintf(s_buf, sizeof s_buf, "%s", initial);
        s_len = (int)strlen(s_buf);
    }

    /* 🚨 Built on the top layer. It covers the calling app without touching
     * its screen, so closing it just lifts away — the app has nothing to
     * rebuild. */
    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   /* stop touches falling through */

    lv_obj_t *t = lv_label_create(s_scr);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x6E7686), 0);
    /* 🚨 At -222 the circle is only 142 px wide and the title was clipped.
     * -206 gives 218 px, which fits. */
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -206);

    /* What you typed. 🚨 Never masked — see the note at the top. */
    s_field = lv_label_create(s_scr);
    lv_obj_set_width(s_field, 156);
    lv_label_set_long_mode(s_field, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_field, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_field, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_text_align(s_field, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_field, LV_ALIGN_CENTER, 0, -172);

    /* Top row: [back] [ typed ] [OK]. 🚨 This screen has somewhere to go back
     * to, so it shows a button rather than relying on the handle alone — the
     * handle is only obvious to people who already know it.
     * Next moved to the bottom row. */
    kp_btn(s_scr, -102, -172, 44, 40, LV_SYMBOL_LEFT, kp_cancel_btn, 0, 0x24242A);
    kp_btn(s_scr,  102, -172, 44, 40, LV_SYMBOL_OK,   kp_ok,        0, 0x2E6E5A);

    /* Nine digits plus a bottom row of four. kp_paint fills in the faces. */
    for (int i = 0; i < 9; i++) {
        int dx = (i % 3 - 1) * 110;
        int dy = -100 + (i / 3) * 72;
        lv_obj_t *b = kp_btn(s_scr, dx, dy, 104, 68, "", kp_key, i + 1, 0x1D1D24);
        s_keylbl[i + 1] = lv_obj_get_child(b, 0);
    }
    /* 🚨 Four on the bottom row. At dy 116 the circle is 178 px half-wide, so
     * four 88 px keys (352) fit. Putting next here frees the top row for the
     * back button. */
    lv_obj_t *mb = kp_btn(s_scr, -132, 116, 88, 68, "abc", kp_mode, 0, 0x24242A);
    s_modelbl = lv_obj_get_child(mb, 0);
    lv_obj_t *zb = kp_btn(s_scr, -44, 116, 88, 68, "", kp_key, 0, 0x1D1D24);
    s_keylbl[0] = lv_obj_get_child(zb, 0);
    kp_btn(s_scr,  44, 116, 88, 68, LV_SYMBOL_RIGHT,     kp_next, 0, 0x24242A);
    kp_btn(s_scr, 132, 116, 88, 68, LV_SYMBOL_BACKSPACE, kp_back, 0, 0x24242A);

    /* Handle up cancels. Same gesture as leaving an app, so nothing to teach. */
    launcher_handle_add(s_scr, kp_cancel);
    kp_paint();          /* 🚨 this is where the key faces first get painted */
}

void keypad_close(void)
{
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_field = s_modelbl = NULL;
    for (int i = 0; i < 10; i++) s_keylbl[i] = NULL;
    s_done = NULL;
}

bool keypad_is_open(void) { return s_scr != NULL; }
