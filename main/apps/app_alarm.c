/* 알람. 시계 앱의 한 쪽이지만, **몸은 앱 밖에 산다.**
 *
 * 🚨 앱 안에만 두면 시계 앱을 닫는 순간 죽는다. 울려야 할 때 앱이 열려 있을
 * 리가 없으니 그건 알람이 아니다. 그래서 시각을 보는 일(alarm_tick)은 런처가
 * 매초 부르고, 이 파일의 static 이 그 사이를 들고 있는다 — LVGL 조각만 앱을
 * 닫을 때 사라지고 상태는 남는다.
 *
 * 🚨 이 보드엔 RTC 칩이 없다(0x51 이 응답하지 않는다). 전원이 완전히 끊기면
 * 시각을 잃고 1970년으로 돌아온다. 그래서 **시각이 안 맞춰졌으면 안 울린다** —
 * 1970년 기준으로 "아침 7시" 를 따지면 꽂자마자 울려댄다.
 *
 * 🚨 keep_awake 는 "꺼지는 걸 막는" 것뿐이라 이미 꺼진 화면은 못 켠다.
 * 타이머가 같은 데서 한 번 당했다 — 켜고, 켜진 채로 붙잡는다.
 */
#include "app.h"
#include "port.h"
#include <time.h>

/* 카시오 알람 흉내. 타이머와 같은 소리를 쓴다 — 배지 안에서 한 소리로 통일. */
static const uint8_t BEEP[] = {
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
};
#define BEEP_HZ   4000
#define BEEP_STEP 70

/* ── 앱을 닫아도 남는 것 ────────────────────────────────────── */
static uint8_t s_h = 7, s_m = 0;
static bool    s_on;
static bool    s_loaded;
static int     s_fired_yday = -1;   /* 오늘 이미 울렸나 — 하루 한 번 */
static int     s_beep_i = -1;       /* -1 = 안 울림 */
static lv_obj_t   *s_ring;          /* 울릴 때 화면을 덮는 판 */
static lv_timer_t *s_beep_timer;

/* ── 앱이 열려 있을 때만 있는 것 ──────────────────────────── */
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

/* ── 울리기 ─────────────────────────────────────────────────── */
static void beep_step(lv_timer_t *t)
{
    if (s_beep_i < 0) {
        port_tone_enable(false);
        lv_timer_delete(t);
        s_beep_timer = NULL;
        return;
    }
    /* 사람이 끌 때까지 운다. 묶음을 처음부터 다시 돈다. */
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
    port_tone_hold(true);          /* 우는 동안 코덱을 붙잡아 첫 소리를 안 놓친다 */
    launcher_screen_on();          /* 🚨 먼저 켠다. 붙잡기만 하면 깜깜한 채 운다 */
    launcher_keep_awake_by(AWAKE_RING, true);

    /* 🚨 어느 앱이 열려 있든 덮어야 한다. 그래서 맨 위 층에 만든다 —
     * 앱이 제 화면을 지워도 이건 안 지워진다. */
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
    port_tone_hold(false);         /* 그친 뒤엔 코덱을 놓아 절전으로 */
    launcher_keep_awake_by(AWAKE_RING, false);
    if (s_ring) { lv_obj_delete(s_ring); s_ring = NULL; }
}

/* ── 런처가 매초 부른다 ─────────────────────────────────────── */
void alarm_tick(void)
{
    load();
    if (!s_on || s_beep_i >= 0) return;

    /* 🚨 시각이 안 맞춰졌으면 안 울린다. RTC 가 없어 전원이 끊기면 1970년
     * 으로 돌아오는데, 거기서 "아침 7시" 를 따지면 꽂자마자 울려댄다.
     * 2001년보다 이르면 안 맞춰진 것으로 본다. */
    time_t now = time(NULL);
    if (now < 978307200) return;

    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_hour != s_h || tm.tm_min != s_m) return;
    if (s_fired_yday == tm.tm_yday) return;     /* 하루 한 번 */
    s_fired_yday = tm.tm_yday;
    start_ring();
}

/* ── 쪽 화면 ────────────────────────────────────────────────── */
static void paint(void)
{
    if (!s_time_lbl) return;
    lv_label_set_text_fmt(s_time_lbl, "%02u:%02u", s_h, s_m);
    /* 🚨 꺼졌을 때를 0x44444E 로 뒀더니 검정 바탕에 묻혀 안 보였다(0910 지적).
     * 꺼진 것도 읽혀야 몇 시로 맞춰뒀는지 안다 — 초록만 아니면 된다. */
    lv_obj_set_style_text_color(s_time_lbl,
        s_on ? lv_color_hex(0x5BD48A) : lv_color_hex(0xA8AEBC), 0);
    lv_label_set_text(s_state_lbl, s_on ? "on" : "off");
    lv_obj_set_style_text_color(s_state_lbl,
        s_on ? lv_color_hex(0x081A10) : lv_color_hex(0xD2D8E4), 0);
    /* 켜짐/꺼짐을 글씨가 아니라 판 색으로 먼저 보이게 한다 — 둥근 화면에서
     * 세 글자를 읽게 하는 것보다 색 덩어리가 빠르다. */
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
    /* 시각을 바꾸면 "오늘 울렸다" 는 기록도 지운다 — 방금 지난 시각으로
     * 맞춰놓고 왜 안 우냐고 하지 않게. */
    s_fired_yday = -1;
    save();
    paint();
}

/* 🚨 판 색이 0x1D1D24 라 검정 바탕에서 버튼이 어디 있는지 안 보였다(0910
 * 지적). 이 화면은 눈으로 겨냥해 누르는 곳이라 테두리까지 줘서 경계를
 * 분명히 한다 — 손가락 자리는 96×72 로 키웠다(전 78×58). */
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

    /* 시 ▲▼ 는 왼쪽, 분 ▲▼ 는 오른쪽. 숫자를 사이에 두어 무엇을 바꾸는지
     * 자리로 알게 한다 — 둥근 화면이라 글로 적을 자리가 아깝다.
     * 🚨 자리는 원 안에서 잡는다. 제일 먼 모서리(-160,-116)가 중심에서 198px
     * 이라 반지름 233 안에 든다 — 넓힐 때마다 이걸 다시 따져야 잘리지 않는다. */
    mk_btn(root, -112, -80, LV_SYMBOL_UP,   0);
    mk_btn(root, -112,  62, LV_SYMBOL_DOWN, 1);
    mk_btn(root,  112, -80, LV_SYMBOL_UP,   2);
    mk_btn(root,  112,  62, LV_SYMBOL_DOWN, 3);

    /* 🚨 토글을 화살표와 같은 줄에 뒀더니 좌우로 7px 씩 물렸다(0910 시뮬).
     * 한 줄 아래로 내린다 — 제일 먼 모서리가 중심에서 198px 이라 안전하다. */
    s_tog = mk_btn(root, 0, 152, "", 4);
    lv_obj_set_size(s_tog, 150, 62);
    s_state_lbl = lv_label_create(s_tog);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_state_lbl);

    /* 🚨 한 라벨에 "hour        min" 으로 넣었더니 두 낱말이 가운데로 몰려
     * 버튼 기둥 위가 아니라 숫자 위에 떴다(0910 시뮬). 기둥마다 하나씩 —
     * 이름은 제가 가리키는 것 바로 위에 있어야 한다. */
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
    /* 🚨 조각만 놓는다. 울리는 것과 맞춰둔 시각은 앱 밖의 것이라 안 건드린다 —
     * 여기서 stop_ring 을 부르면 시계 앱을 닫는 순간 알람이 그친다. */
    s_time_lbl = s_state_lbl = s_hint = s_tog = NULL;
}
