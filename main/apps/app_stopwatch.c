/* 스톱워치. 시계 앱의 한 쪽으로 산다(타일뷰에서 위아래로 넘긴다).
 *
 * 타이머가 "남은 시간을 링으로 본다" 면 이쪽은 숫자가 주인공이다 — 재는 게
 * 목적이라 눈이 숫자에 붙는다. 그래서 링은 안 그리고 큰 글씨만 둔다.
 *
 * 🚨 도는 동안 화면이 꺼지면 안 된다(0910 지적). 다만 **도는 동안만** 붙잡는다.
 * 아예 못 꺼지게 두면 책상에 올려놔도 화면이 살아 배터리를 태운다 — 물 앱에서
 * 같은 판단을 했다. 멈추면 곧바로 놓는다.
 *
 * 🚨 숫자를 라벨 하나에 통째로 넣으면 안 된다(0910 지적). 몽세라는 글자마다
 * 폭이 다른 글꼴이라 1 과 8 의 폭이 다르다. 가운데 맞춤 라벨 하나에 넣으면
 * 100분의 1초가 바뀔 때마다 전체 폭이 달라져 숫자가 좌우로 덜덜 떤다.
 * 글자마다 제 칸을 주고 칸 폭을 숫자 최대폭으로 고정한다 — 자리는 안 움직이고
 * 글자만 바뀐다. 아래 row_t 가 그 일을 한다.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

/* ── 고정폭 숫자 줄 ─────────────────────────────────────────── */
#define SLOT_N 8

typedef struct {
    lv_obj_t *slot[SLOT_N];
    char      ch[SLOT_N];      /* 지금 그 칸에 든 글자 — 안 바뀌면 안 건드린다 */
    int16_t   dw, sw;          /* 숫자 칸 폭, 구분표(: .) 칸 폭 */
    int16_t   dx, dy;
    uint32_t  sig;             /* 칸 배치가 실제로 바뀌었을 때만 다시 놓는다 */
    uint32_t  col;
} row_t;

static void row_measure(row_t *r, const lv_font_t *f, int pad)
{
    /* 칸 폭은 글꼴에서 직접 잰다. 숫자는 그중 제일 넓은 것에 맞춘다 —
     * 그래야 어떤 숫자가 와도 칸이 안 흔들린다. */
    uint16_t dw = 0;
    for (uint32_t c = '0'; c <= '9'; c++) {
        uint16_t w = lv_font_get_glyph_width(f, c, 0);
        if (w > dw) dw = w;
    }
    uint16_t sw = lv_font_get_glyph_width(f, ':', 0);
    uint16_t pw = lv_font_get_glyph_width(f, '.', 0);
    if (pw > sw) sw = pw;
    /* pad 는 칸 사이를 벌리는 여유다. 글자 폭에 딱 맞추면 큰 글꼴에서 숫자가
     * 서로 붙어 답답해 보인다 — 큰 줄일수록 넉넉하게. */
    r->dw = (int16_t)(dw + pad);
    r->sw = (int16_t)(sw + pad);
}

static void row_make(row_t *r, lv_obj_t *root, const lv_font_t *f,
                     int dx, int dy, int pad, uint32_t col)
{
    memset(r, 0, sizeof *r);
    r->dx = (int16_t)dx;
    r->dy = (int16_t)dy;
    r->col = col;
    row_measure(r, f, pad);

    for (int i = 0; i < SLOT_N; i++) {
        lv_obj_t *l = lv_label_create(root);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, "");
        r->slot[i] = l;
    }
}

/* 여덟 글자까지. 숫자면 넓은 칸, : 나 . 이면 좁은 칸. */
static void row_set(row_t *r, const char *s)
{
    if (!r->slot[0]) return;

    int n = 0;
    while (n < SLOT_N && s[n]) n++;

    int wid[SLOT_N], total = 0;
    uint32_t sig = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        int d = (s[i] >= '0' && s[i] <= '9');
        wid[i] = d ? r->dw : r->sw;
        total += wid[i];
        sig = sig * 3u + (uint32_t)(d ? 1 : 2);
    }
    bool relay = (sig != r->sig);
    r->sig = sig;

    int x = r->dx - total / 2;
    for (int i = 0; i < SLOT_N; i++) {
        lv_obj_t *l = r->slot[i];
        if (i >= n) {
            if (r->ch[i]) { lv_label_set_text(l, ""); r->ch[i] = 0; }
            continue;
        }
        if (relay) {
            lv_obj_set_width(l, wid[i]);
            lv_obj_align(l, LV_ALIGN_CENTER, x + wid[i] / 2, r->dy);
        }
        if (r->ch[i] != s[i]) {
            char t[2] = { s[i], 0 };
            lv_label_set_text(l, t);
            r->ch[i] = s[i];
        }
        x += wid[i];
    }
}

static void row_color(row_t *r, uint32_t col)
{
    if (!r->slot[0] || r->col == col) return;
    r->col = col;
    for (int i = 0; i < SLOT_N; i++)
        lv_obj_set_style_text_color(r->slot[i], lv_color_hex(col), 0);
}

/* 여덟 글자짜리 한 줄의 폭. "lap" 을 그 왼쪽에 붙일 때 쓴다. */
static int row_width8(const row_t *r) { return 6 * r->dw + 2 * r->sw; }

/* ── 상태 ───────────────────────────────────────────────────── */
static row_t       s_bigrow, s_laprow;
static lv_obj_t   *s_lapword, *s_hint;
static lv_timer_t *s_tick;

static bool     s_run;
static uint32_t s_base_ms;      /* 돌기 시작한 시각 */
static uint32_t s_acc_ms;       /* 멈춰 있던 동안 쌓아둔 것 */
static uint32_t s_lap_ms;       /* 마지막으로 끊은 자리 */
static uint32_t s_press_ms;
static bool     s_long_done;

static uint32_t elapsed(void)
{
    /* 🚨 lv_tick_get() 은 32비트 밀리초라 49일에 한 번 넘친다. 뺄셈은 넘쳐도
     * 맞으므로(부호 없는 산술) 이 식은 그대로 옳다 — 값을 직접 견주지 말 것. */
    return s_acc_ms + (s_run ? (lv_tick_get() - s_base_ms) : 0);
}

static void paint(void)
{
    uint32_t ms = elapsed();
    uint32_t cs = (ms / 10) % 100;
    uint32_t ss = (ms / 1000) % 60;
    uint32_t mm = (ms / 60000) % 60;
    uint32_t hh =  ms / 3600000;
    if (hh > 99) hh = 99;          /* 여덟 칸을 넘기지 않는다 */

    char buf[16];
    if (hh) snprintf(buf, sizeof buf, "%lu:%02lu:%02lu",
                     (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
    else    snprintf(buf, sizeof buf, "%02lu:%02lu.%02lu",
                     (unsigned long)mm, (unsigned long)ss, (unsigned long)cs);
    row_set(&s_bigrow, buf);

    if (s_lap_ms) {
        uint32_t d = ms - s_lap_ms;
        snprintf(buf, sizeof buf, "%02lu:%02lu.%02lu",
                 (unsigned long)((d / 60000) % 60),
                 (unsigned long)((d / 1000) % 60),
                 (unsigned long)((d / 10) % 100));
        row_set(&s_laprow, buf);
        if (s_lapword) lv_obj_remove_flag(s_lapword, LV_OBJ_FLAG_HIDDEN);
    } else {
        row_set(&s_laprow, "");
        if (s_lapword) lv_obj_add_flag(s_lapword, LV_OBJ_FLAG_HIDDEN);
    }

    const char *h = s_run ? "tap to stop - hold to lap"
                  : (ms ? "tap to go - hold to reset" : "tap to start");
    lv_label_set_text(s_hint, h);

    row_color(&s_bigrow, s_run ? 0x5BD48A : (ms ? 0xE8C46B : 0x7FB0FF));
}

static void tick(lv_timer_t *t)
{
    (void)t;
    /* 🔋 안 돌면 다시 칠할 이유가 없다. 화면이 꺼졌어도 마찬가지다 —
     * 다만 시간은 계속 흐른다(elapsed 가 시계에서 뽑으므로 안 밀린다). */
    if (!s_run) return;
    if (launcher_screen_is_off()) return;
    paint();
}

static void set_run(bool on)
{
    if (on == s_run) return;
    if (on) {
        s_base_ms = lv_tick_get();
    } else {
        s_acc_ms = elapsed();
    }
    s_run = on;
    /* 🚨 도는 동안만 붙잡는다. 여럿이 동시에 잡을 수 있으므로 제 몫으로만
     * 잡는다 — 타이머 알람이 울리는 중에 스톱워치를 멈춰도 알람 쪽이 안 풀린다. */
    launcher_keep_awake_by(AWAKE_STOP, s_run);
    paint();
}

static void press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) { s_press_ms = lv_tick_get(); s_long_done = false; return; }

    if (code == LV_EVENT_PRESSING) {
        if (s_long_done || lv_tick_get() - s_press_ms < 600) return;
        s_long_done = true;
        if (s_run) {
            s_lap_ms = elapsed();          /* 돌 때 길게 = 랩 */
        } else {
            s_acc_ms = 0; s_lap_ms = 0;    /* 멈췄을 때 길게 = 되돌리기 */
        }
        paint();
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (!s_long_done) set_run(!s_run);
    }
}

void stopwatch_build(lv_obj_t *root)
{
    s_run = false;
    s_acc_ms = 0;
    s_lap_ms = 0;
    s_long_done = false;

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* 재는 게 목적인 화면이라 숫자를 있는 대로 키운다 — 48이 이 펌웨어에
     * 들어 있는 제일 큰 글꼴이다. 칸을 6px 씩 벌려 답답하지 않게. */
    row_make(&s_bigrow, root, &lv_font_montserrat_48, 0, -26, 6, 0x7FB0FF);

    /* 랩은 "lap" 을 왼쪽에 붙이고 숫자는 그 오른쪽에. 숫자 줄은 늘 여덟 칸이라
     * 폭이 정해져 있어서 둘을 미리 붙여 놓을 수 있다 — 랩이 바뀌어도 "lap" 이
     * 안 밀린다. */
    lv_point_t wsz;
    lv_text_get_size(&wsz, "lap", &lv_font_montserrat_24, 0, 0, LV_COORD_MAX, 0);
    row_t probe;
    memset(&probe, 0, sizeof probe);
    row_measure(&probe, &lv_font_montserrat_24, 3);
    int numw = row_width8(&probe);
    int gap  = 14;
    int left = -(wsz.x + gap + numw) / 2;

    s_lapword = lv_label_create(root);
    lv_label_set_text(s_lapword, "lap");
    lv_obj_set_style_text_font(s_lapword, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lapword, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(s_lapword, LV_ALIGN_CENTER, left + wsz.x / 2, 46);
    lv_obj_add_flag(s_lapword, LV_OBJ_FLAG_HIDDEN);

    row_make(&s_laprow, root, &lv_font_montserrat_24,
             left + wsz.x + gap + numw / 2, 46, 3, 0x8A93A6);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 122);

    /* 🚨 타일뷰 안이라 위아래 쓸기가 쪽 넘기기로 먼저 먹힌다. 누름만 받는다. */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_RELEASED, NULL);

    /* 100분의 1초가 눈에 흐르게 하려면 이보다 느리면 안 된다. 도는 동안만
     * 실제로 일하므로(위 tick) 멈춰 있을 때의 값은 0 이다. */
    s_tick = lv_timer_create(tick, 47, NULL);
    paint();
}

void stopwatch_free(void)
{
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    /* 🚨 나가면 놓는다. 안 놓으면 시계 앱을 닫아도 화면이 안 꺼진다. */
    launcher_keep_awake_by(AWAKE_STOP, false);
    s_run = false;
    /* 🚨 조각은 타일과 함께 이미 지워졌다. 가리키던 것만 지운다 — 남겨두면
     * 다음에 열 때 없어진 자리를 만진다. */
    memset(&s_bigrow, 0, sizeof s_bigrow);
    memset(&s_laprow, 0, sizeof s_laprow);
    s_lapword = s_hint = NULL;
}
