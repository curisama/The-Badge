/* 회의 녹음. 배지가 혼자 녹음한다.
 *
 * 회사 회의실엔 WiFi 도 없고 폰을 꺼낼 틈도 없다. 그래서 이 앱은 아무데도
 * 안 붙는다 — 마이크를 열어 플래시에 쌓기만 한다. 집에 와서 WiFi 가 잡히면
 * 그때 홈서버로 올라가고, 전사·화자분리·회의록은 거기가 다 한다.
 *
 * 녹음은 앱을 나가도, 화면이 꺼져도 계속된다(별도 태스크). 그래야 회의 중에
 * 배지를 주머니에 넣어둘 수 있다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>

typedef enum { M_IDLE, M_REC, M_SAVED, M_FAIL } mstate_t;

static lv_obj_t   *s_ring, *s_btn, *s_big, *s_sub, *s_hint;
static lv_timer_t *s_tick;
static mstate_t    s_st;
static bool        s_english;
static const char *s_why;

static void paint(void)
{
    static const uint32_t COL[] = {
        0x7FB0FF,   /* IDLE  파랑 */
        0xFF5B5B,   /* REC   빨강 */
        0x5BD48A,   /* SAVED 초록 */
        0xFF6B6B,   /* FAIL  빨강 */
    };
    lv_color_t c = lv_color_hex(COL[s_st]);

    char big[16], sub[40];
    const char *hint = "";
    int secs = (int)port_rec_seconds();

    switch (s_st) {
    case M_IDLE: {
        snprintf(big, sizeof big, "REC");
        uint32_t freem = port_rec_free_seconds() / 60;
        int pend = port_rec_pending();
        if (pend > 0) snprintf(sub, sizeof sub, "%d saved", pend);
        else          snprintf(sub, sizeof sub, "%lu min free", (unsigned long)freem);
        hint = "hold = export";
        break;
    }
    case M_REC:
        snprintf(big, sizeof big, "%d:%02d", secs / 60, secs % 60);
        snprintf(sub, sizeof sub, "recording");
        hint = "tap = stop";
        break;
    case M_SAVED:
        snprintf(big, sizeof big, "OK");
        snprintf(sub, sizeof sub, "saved  %d on device", port_rec_pending());
        hint = "export over USB";
        break;
    default:
        snprintf(big, sizeof big, "!");
        snprintf(sub, sizeof sub, "%s", s_why ? s_why : "failed");
        hint = "tap = retry";
        break;
    }

    lv_label_set_text(s_big, big);
    lv_label_set_text(s_sub, sub);
    lv_label_set_text(s_hint, hint);
    lv_obj_set_style_text_color(s_big, c, 0);
    lv_obj_set_style_border_color(s_btn, c, 0);
    lv_obj_set_style_arc_color(s_ring, c, LV_PART_INDICATOR);

    /* 녹음 중엔 링이 1분에 한 바퀴. 초침 대신이다. */
    lv_arc_set_value(s_ring, s_st == M_REC ? (secs % 60) * 100 / 60 : 100);
}

static void go(mstate_t st)
{
    s_st = st;
    /* 녹음 중에 화면을 붙잡으면 안 된다. 466x466 AMOLED 를 한 시간 켜두는 건
     * 마이크보다 훨씬 크다. 녹음은 태스크가 도니 화면과 무관하다. */
    launcher_keep_awake(false);
    paint();
}

static void start(bool english)
{
    s_english = english;
    if (!port_rec_start(english ? 1 : 0)) {
        s_why = port_rec_capacity() ? "no space / mic failed" : "no recording partition";
        go(M_FAIL);
        return;
    }
    go(M_REC);
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    switch (s_st) {
    case M_IDLE: start(false); break;
    case M_REC:   port_rec_stop(); go(M_SAVED); break;
    case M_SAVED:
    case M_FAIL:  go(M_IDLE);                   break;
    }
}

static void long_cb(lv_event_t *e)
{
    (void)e;
    if (s_st == M_IDLE) start(true);
}

static void tick(lv_timer_t *t)
{
    /* 🔋 화면이 꺼지면 아무도 안 본다. 다만 🚨 여기서 주기를 바꾸면 안 된다 —
     * lv_timer_set_period() 는 안쪽에서 lv_timer_handler_resume() 을 불러서,
     * 타이머 콜백에서 부르면 처리기가 그 자리에서 무한히 다시 돈다.
     * (0909: 절전하려고 넣었다가 CPU 를 100% 물고 늘어지게 만들었다.
     *  값이 같아도 마찬가지라 "바뀔 때만 세우기"로도 못 막는다.)
     * 주기는 그대로 두고 6번에 한 번만 일한다. 효과는 같고 안전하다. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 6) return;
    }

    /* 녹음이 스스로 멈췄으면(자리 참) 화면도 따라간다 */
    if (s_st == M_REC && !port_rec_active()) { go(M_SAVED); return; }
    /* 대기 중엔 바뀌는 게 없다. 예전엔 초당 2번 화면 86%를 헛되이
     * 무효화했다 — 녹음 중일 때만 그린다(경과 시간이 흐르니까). */
    if (s_st == M_REC) paint();
}

/* 🚨 내보내기는 **녹음이 멈춰 있을 때만** 연다. 녹음 중에 USB 모드로
 * 넘어가면 "끝" 이 재부팅이라 담고 있던 것이 잘린다. */
static void usb_cb(lv_event_t *e)
{
    (void)e;
    if (port_rec_active()) return;
    usb_screen_open();
}

static void meet_enter(lv_obj_t *root)
{
    port_crumb(CRUMB_MEET);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0A0D14), 0);

    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, 430, 430);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 100);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x1E2634), LV_PART_MAIN);

    /* 가운데 큰 버튼. 원형 화면에선 이게 제일 누르기 편하다. */
    s_btn = lv_obj_create(root);
    lv_obj_set_size(s_btn, 300, 300);
    lv_obj_center(s_btn);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x141A26), 0);
    lv_obj_set_style_border_width(s_btn, 4, 0);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn, tap_cb,  LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn, long_cb, LV_EVENT_LONG_PRESSED,  NULL);

    s_big = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_big, &lv_font_montserrat_48, 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -18);

    s_sub = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0xB6C2D6), 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 34);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A6478), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 178);

    /* 케이블로 꺼내는 문. 🚨 가운데 큰 버튼 위에 겹치면 녹음을 누르려다
     * 이걸 누른다 — 위쪽 귀퉁이로 뺀다(원 안쪽이라 dy -168, dx 108). */
    lv_obj_t *ub = lv_button_create(root);
    lv_obj_set_size(ub, 64, 40);
    lv_obj_set_style_radius(ub, 20, 0);
    lv_obj_set_style_bg_color(ub, lv_color_hex(0x1B2333), 0);
    lv_obj_set_style_shadow_width(ub, 0, 0);
    lv_obj_align(ub, LV_ALIGN_CENTER, 108, -168);
    lv_obj_add_event_cb(ub, usb_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ul = lv_label_create(ub);
    lv_label_set_text(ul, LV_SYMBOL_DRIVE);
    lv_obj_set_style_text_color(ul, lv_color_hex(0x9FB3CC), 0);
    lv_obj_center(ul);

    /* 나갔다 들어와도 녹음이 돌고 있으면 그 화면으로 돌아온다 */
    s_st = port_rec_active() ? M_REC : M_IDLE;
    s_why = NULL;
    s_tick = lv_timer_create(tick, 500, NULL);
    paint();
}

static void meet_leave(void)
{
    launcher_keep_awake(false);
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    /* 녹음은 안 멈춘다. 회의 중에 다른 앱을 봐도 계속 담겨야 한다. */
}

static lv_color_t meet_tint(void) { return lv_color_hex(0xFF5B5B); }

const badge_app_t app_meet = {
    .name = "Meet", .art = &app_icon_meet, .icon = LV_SYMBOL_AUDIO,
    .tint = meet_tint, .radio = RADIO_OFF, .keep_awake = false,
    .enter = meet_enter, .leave = meet_leave,
};
