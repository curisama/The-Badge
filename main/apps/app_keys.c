/* F키 다이얼.
 *
 * 폰 소프트 키보드엔 F키가 없다. 그게 폰 RDP 로 엑셀 수식을 못 만지는
 * 진짜 이유다 — F4(절대참조 토글) 하나가 없어서 수식 체인을 못 짠다.
 * 원형 화면에 방사형으로 깔고 눌러서 쏜다.
 *
 * 애플 터치바와 다른 점: 터치바는 있던 물리 F키를 뺏어서 욕먹었다.
 * 이건 원래 없던 걸 더한다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <math.h>

/* USB HID 키코드 */
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

/* 엑셀에서 손이 제일 자주 가는 것들. 순서는 화면 12시부터 시계방향. */
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
    /* 🔋 화면이 꺼지면 아무도 안 본다. 다만 🚨 여기서 주기를 바꾸면 안 된다 —
     * lv_timer_set_period() 는 안쪽에서 lv_timer_handler_resume() 을 불러서,
     * 타이머 콜백에서 부르면 처리기가 그 자리에서 무한히 다시 돈다.
     * (0909: 절전하려고 넣었다가 CPU 를 100% 물고 늘어지게 만들었다.
     *  값이 같아도 마찬가지라 "바뀔 때만 세우기"로도 못 막는다.)
     * 주기는 그대로 두고 4번에 한 번만 일한다. 효과는 같고 안전하다. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 4) return;
    }

    /* 연결 상태가 그대로면 다시 그릴 게 없다. LVGL 은 값이 같아도
     * 스타일을 세우면 무조건 무효화한다. */
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
    /* 12개를 30도 간격으로. 가운데는 비워서 뭘 눌렀는지 보여준다. */
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

/* 셋 다 키보드를 쏘는 물건이라 한 앱으로 묶고 위아래로 넘긴다.
 * 홈에 아이콘을 세 개 두는 것보다 이쪽이 찾기 쉽다. */
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
    /* 화면을 보며 쓰는 앱이라 무한정 붙잡을 근거가 약하다. 30초 뒤 꺼져도
 * PWR 한 번이면 돌아온다 — 켬/끔 차이가 시간당 124mV 다. */
    .radio = RADIO_BLE, .keep_awake = false, .enter = enter, .leave = leave,
};
