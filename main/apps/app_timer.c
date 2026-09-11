/* 타이머. 원형 화면이 가장 잘 쓰이는 용도 중 하나 —
 * 남은 시간을 숫자로 읽는 게 아니라 링이 줄어드는 걸로 본다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"

static const int PRESET[3] = { 5, 15, 25 };     /* 분 */

static lv_obj_t   *s_arc, *s_time, *s_hint;
static lv_obj_t   *s_chip[3];
static lv_timer_t *s_tick;

static int  s_total;        /* 초 */
static int  s_left;
static bool s_running;
/* 카시오 알람 흉내. 4kHz 짧은 소리 두 번, 쉬고, 두 번 — 네 묶음.
 * 1 = 소리, 0 = 침묵. 한 칸이 70ms 다. */
static const uint8_t BEEP[] = {
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
    1,0,1,0,0,0,0,0,
};
#define BEEP_HZ   4000
#define BEEP_STEP 70
static int s_beep_i = -1;   /* -1 = 안 울림 */
static uint32_t s_press_ms;
static bool     s_long_done;

static void paint(void)
{
    /* 멈춰 있을 땐 링이 "설정한 분"을, 돌 때는 "남은 비율"을 보여준다 */
    if (s_running || s_left != s_total) {
        lv_arc_set_range(s_arc, 0, 1000);
        lv_arc_set_value(s_arc, s_total ? s_left * 1000 / s_total : 0);
    } else {
        lv_arc_set_range(s_arc, 1, 90);
        lv_arc_set_value(s_arc, s_total / 60);
    }
    lv_label_set_text_fmt(s_time, "%d:%02d", s_left / 60, s_left % 60);

    const char *h = s_left == 0 ? (s_beep_i >= 0 ? "tap to stop" : "done")
                  : (s_running ? "tap to pause" : "tap to start - hold to reset");
    lv_label_set_text(s_hint, h);

    lv_color_t c = s_left == 0 ? lv_color_hex(0xFF6B6B)
                 : s_running   ? lv_color_hex(0x5BD48A)
                               : lv_color_hex(0x7FB0FF);
    lv_obj_set_style_arc_color(s_arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_time, c, 0);
}

/* 소리 묶음은 1초 타이머로는 못 낸다. 따로 빠른 타이머를 둔다. */
static void beep_step(lv_timer_t *t)
{
    if (s_beep_i < 0) {
        port_tone_enable(false);
        lv_timer_delete(t);
        return;
    }
    /* 🚨 예전엔 묶음을 한 번 울리고 스스로 그쳤다(0908 실기: 네 번만 남).
     * 알람은 사람이 끌 때까지 울려야 알람이다. 묶음을 처음부터 다시 돈다. */
    if (s_beep_i >= (int)sizeof(BEEP)) s_beep_i = 0;
    port_tone_freq(BEEP_HZ);
    port_tone_enable(BEEP[s_beep_i] != 0);
    s_beep_i++;
}

static void beep_start(void)
{
    if (s_beep_i >= 0) return;
    s_beep_i = 0;
    port_tone_hold(true);        /* 우는 동안 코덱을 붙잡아 첫 소리를 안 놓친다 */
    /* 🚨 keep_awake 는 "꺼지는 걸 막는" 것뿐이라, 이미 꺼진 화면은 못 켠다.
     * 25분 타이머를 걸면 그 사이 화면이 꺼지고, 알람이 울려도 깜깜한 채라
     * 어디를 눌러야 그치는지 알 수가 없었다. 켜고, 켜진 채로 붙잡는다. */
    launcher_screen_on();
    launcher_keep_awake(true);
    lv_timer_create(beep_step, BEEP_STEP, NULL);
}

static void beep_stop(void)
{
    s_beep_i = -1;
    port_tone_enable(false);
    port_tone_hold(false);       /* 그친 뒤엔 코덱을 놓아 절전으로 */
}

static void tick(lv_timer_t *t)
{
    (void)t;
    /* 멈춰 있으면 매초 420px 호를 다시 칠할 이유가 없다 */
    if (!s_running && s_left != 0 && s_beep_i < 0) return;
    if (s_running && s_left > 0) {
        /* 🚨 붙잡을지는 탭할 때 한 번만 정했다. 25분을 걸면 그때는 1분이
         * 아니니 안 붙잡고, 이후로 다시 볼 일이 없었다. 매초 다시 본다. */
        if (s_left == 61) launcher_keep_awake(true);
        if (--s_left == 0) {
            s_running = false;
            launcher_keep_awake(false);
            beep_start();
        }
    }
    paint();
}

/* 문지르다 손을 떼도 CLICKED 가 온다. 움직였는지 직접 본다. */
static lv_point_t s_press_pt;
static bool       s_dragged;

static void hit_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_press_pt = p;
        s_dragged = false;
        s_press_ms = lv_tick_get();
        s_long_done = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (LV_ABS(p.x - s_press_pt.x) > 8 || LV_ABS(p.y - s_press_pt.y) > 8) s_dragged = true;
        /* 길게 누르면 리셋. 떼는 걸 기다리지 않고 그 자리에서 해준다 —
         * 그래야 손가락에 반응이 온다. */
        if (!s_dragged && !s_long_done && lv_tick_get() - s_press_ms >= 600) {
            s_long_done = true;
            s_running = false;
            s_left = s_total;
            beep_stop();
            launcher_keep_awake(false);
            paint();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_dragged || s_long_done) return; /* 문지른 것·길게 누른 것은 시작이 아니다 */
        if (s_left == 0) { s_left = s_total; beep_stop(); }
        else             { s_running = !s_running; }
        /* 🚨 예전엔 도는 내내 붙잡았다. 90분을 잡으면 90분 화면이 켜져 있다.
         * 그런데 화면을 꺼도 s_tick 은 계속 돌고 알람도 울린다(screen_off 는
         * 앱 타이머를 안 재운다). 그러니 끝나가는 1분만 붙잡으면 된다. */
        launcher_keep_awake(s_running && s_left <= 60);
        paint();
    }
}

/* 가장자리 링을 돌려 시간을 잡는다. 돌아가는 중엔 못 바꾼다. */
static void dial_cb(lv_event_t *e)
{
    if (s_running) { lv_arc_set_value(s_arc, s_total ? s_left * 1000 / s_total : 0); return; }
    int min = lv_arc_get_value(lv_event_get_target(e));
    if (min < 1) min = 1;
    s_total = min * 60;
    s_left  = s_total;
    beep_stop();
    lv_label_set_text_fmt(s_time, "%d:%02d", s_left / 60, s_left % 60);
    lv_label_set_text(s_hint, "tap to start");
}

static void preset_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_total = PRESET[i] * 60;
    s_left  = s_total;
    s_running = false;
    beep_stop();
    for (int k = 0; k < 3; k++) {
        lv_obj_set_style_bg_color(s_chip[k],
            lv_color_hex(k == i ? 0x3A5A7A : 0x24242A), 0);
    }
    paint();
}

void timer_build(lv_obj_t *root)
{
    s_total = PRESET[2] * 60;
    s_left  = s_total;
    s_running = false;
    s_beep_i = -1;

    s_arc = lv_arc_create(root);
    lv_obj_set_size(s_arc, 420, 420);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 1, 90);
    lv_arc_set_value(s_arc, s_total / 60);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1E1E22), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 10, LV_PART_KNOB);
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_ADV_HITTEST);   /* 링 띠 위에서만 반응 */
    lv_obj_add_event_cb(s_arc, dial_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 가운데를 통째로 누름판으로 쓴다. 작은 버튼을 겨냥할 필요가 없다. */
    lv_obj_t *hit = lv_button_create(root);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 250, 170);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hit, hit_cb, LV_EVENT_RELEASED, NULL);

    s_time = lv_label_create(root);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -30);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6E6E72), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 18);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = lv_button_create(root);
        lv_obj_set_size(c, 74, 44);
        lv_obj_set_style_radius(c, 22, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(i == 2 ? 0x3A5A7A : 0x24242A), 0);
        lv_obj_set_style_shadow_width(c, 0, 0);
        lv_obj_align(c, LV_ALIGN_CENTER, (i - 1) * 84, 120);
        lv_obj_add_event_cb(c, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text_fmt(l, "%d", PRESET[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
        s_chip[i] = c;
    }

    s_tick = lv_timer_create(tick, 1000, NULL);
    paint();
}

void timer_free(void)
{
    launcher_keep_awake(false);
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    beep_stop();
    s_running = false;      /* 나가면 멈춘다. 백그라운드로 돌릴 만한 물건이 아니다 */
}

