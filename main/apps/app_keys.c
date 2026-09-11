/* The F-key dial.
 *
 * A phone's soft keyboard has no F keys. That is the real reason Excel
 * formulas cannot be touched over RDP from a phone — without F4 (toggling
 * absolute references) there is no building a formula chain. They are laid out
 * radially on the round screen and fired on a press.
 *
 * Unlike Apple's Touch Bar: that took away physical F keys people already had,
 * and was hated for it. This adds ones that were never there. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <math.h>

/* USB HID key codes */
#define K_F1   0x3A
#define K_F2   0x3B
#define K_F4   0x3D
#define K_F5   0x3E
#define K_F12  0x45
#define K_ESC  0x29
#define K_TAB  0x2B
#define K_ENT  0x28
#define K_SEMI 0x33      /* ; */
#define K_LBRK 0x2F      /* [ */
#define K_DEL  0x4C

#define MOD_CTRL   1
#define MOD_SHIFT  2

typedef struct {
    const char *label;
    unsigned    mod;
    unsigned    code;
    const char *hint;
} key_t;

/* The ones Excel reaches for most. The order runs clockwise from 12 o'clock. */
static const key_t KEYS[] = {
    { "F4",  0,         K_F4,   "absolute ref" },
    { "F2",  0,         K_F2,   "edit cell" },
    { "F5",  0,         K_F5,   "go to" },
    { "F12", 0,         K_F12,  "save as" },
    { "Esc", 0,         K_ESC,  "cancel" },
    { "Tab", 0,         K_TAB,  "" },
    { "Ent", 0,         K_ENT,  "" },
    { "^;",  MOD_CTRL,  K_SEMI, "today" },
    { "^[",  MOD_CTRL,  K_LBRK, "trace prec" },
    { "Del", 0,         K_DEL,  "" },
    { "F1",  0,         K_F1,   "help" },
    { "^F4", MOD_CTRL,  K_F4,   "close" },
};
#define KEY_CNT (sizeof(KEYS) / sizeof(KEYS[0]))

static lv_obj_t   *s_hint, *s_state;
static lv_timer_t *s_poll;

static void key_cb(lv_event_t *e)
{
    const key_t *k = (const key_t *)lv_event_get_user_data(e);
    port_hid_key(k->mod, k->code);
    lv_label_set_text_fmt(s_hint, "%s%s%s", k->label,
                          k->hint[0] ? "  " : "", k->hint);
}

static void poll_cb(lv_timer_t *t)
{
    /* 🔋 Screen off, nobody is looking. But 🚨 the period must not be changed
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so calling it from a timer callback sends the handler round again on the
     * spot, forever.
     * (09-09: added to save power, and it pinned the CPU at 100%. Setting the
     *  same value does it too, so "only set it when it changes" does not help.)
     * The period stays as it is and the work happens on one tick in four. Same
     * effect, and safe. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 4) return;
    }

    /* Nothing to redraw while the connection state is unchanged. LVGL
     * invalidates unconditionally when a style is set, same value or not. */
    static int s_prev = -1;
    int now = port_hid_connected() ? 1 : 0;
    if (now == s_prev) return;
    s_prev = now;
    bool on = port_hid_connected();
    lv_label_set_text(s_state, on ? "" : "not connected");
    lv_obj_set_style_text_color(s_state, lv_color_hex(0xE0B33A), 0);
}

static void fkeys_build(lv_obj_t *root)
{
    /* Twelve of them, 30 degrees apart. The middle is left clear to show what was pressed. */
    for (unsigned i = 0; i < KEY_CNT; i++) {
        float ang = (float)(-M_PI / 2.0 + i * (2.0 * M_PI / KEY_CNT));
        int x = (int)(cosf(ang) * 168.f);
        int y = (int)(sinf(ang) * 168.f);

        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 76, 60);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(KEYS[i].mod ? 0x2E4A66 : 0x24242A), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x4A7AA8), LV_STATE_PRESSED);
        lv_obj_align(b, LV_ALIGN_CENTER, x, y);
        lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)&KEYS[i]);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, KEYS[i].label);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }

    s_hint = lv_label_create(root);
    lv_label_set_text(s_hint, "Excel keys");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, -12);

    s_state = lv_label_create(root);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_16, 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, 20);

    s_poll = lv_timer_create(poll_cb, 500, NULL);
    poll_cb(NULL);
}

/* All three send keystrokes, so they are one app swiped vertically.
 * Easier to find that way than three icons on the home screen. */
void type_build(lv_obj_t *root);
void present_build(lv_obj_t *root);
void present_free(void);

static void enter(lv_obj_t *root)
{
    lv_obj_t *tv = lv_tileview_create(root);
    lv_obj_set_size(tv, 466, 466);
    lv_obj_center(tv);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    fkeys_build(lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM));
    type_build(lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM));
    present_build(lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP));
}

static void leave(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    present_free();
}

static lv_color_t tint(void) { return lv_color_hex(0x8AB4F8); }

const badge_app_t app_keys = {
    .name = "Keys", .art = &app_icon_keys, .icon = LV_SYMBOL_KEYBOARD, .tint = tint,
    /* This app is used while looking at it, so there is little ground for
     * holding the screen on indefinitely. Going dark after 30 seconds is one
     * press of PWR away — on versus off is 124 mV an hour. */
    .radio = RADIO_BLE, .keep_awake = false, .enter = enter, .leave = leave,
};
