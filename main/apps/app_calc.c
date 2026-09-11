/* 계산기. 폰 꺼내 계산기 앱 찾는 게 귀찮아서 만들었다.
 *
 * 원형 화면에 4x4 격자를 깔면 네 모서리가 잘린다. 숫자는 가운데 3열로 쌓고
 * 연산자는 원의 좌우 옆구리에 붙였다 — 둥근 화면에서 남는 자리가 거기다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>
#include <math.h>
#define DEG2RAD 0.0174533f

static lv_obj_t *s_disp, *s_sub;

static double s_acc;        /* 쌓인 값 */
static double s_cur;        /* 지금 치는 값 */
static double s_frac;       /* 소수점 자리 (0 이면 정수부 입력 중) */
static char   s_op;         /* 대기 중인 연산자 */
static bool   s_fresh;      /* 다음 숫자를 새로 시작하나 */

static void show(double v)
{
    char buf[32];
    if (fabs(v) >= 1e9 || (v != 0 && fabs(v) < 1e-6)) {
        snprintf(buf, sizeof(buf), "%.4g", v);
    } else if (v == floor(v)) {
        snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        snprintf(buf, sizeof(buf), "%.6g", v);
    }
    lv_label_set_text(s_disp, buf);
}

static void show_sub(void)
{
    if (s_op) lv_label_set_text_fmt(s_sub, "%c", s_op);
    else      lv_label_set_text(s_sub, "");
}

static double apply(double a, double b, char op)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? 0 : a / b;   /* 0 으로 나누면 0. 죽는 것보단 낫다 */
    }
    return b;
}

static void digit(int d)
{
    if (s_fresh) { s_cur = 0; s_frac = 0; s_fresh = false; }
    if (s_frac > 0) { s_cur += d * s_frac; s_frac /= 10.0; }
    else            { s_cur = s_cur * 10 + d; }
    show(s_cur);
}

static void op_key(char op)
{
    if (s_op && !s_fresh) s_cur = apply(s_acc, s_cur, s_op);
    s_acc = s_cur;
    s_op = op;
    s_fresh = true;
    show(s_cur);
    show_sub();
}

static void equals(void)
{
    if (!s_op) return;
    s_cur = apply(s_acc, s_cur, s_op);
    s_op = 0;
    s_fresh = true;
    show(s_cur);
    show_sub();
}

static void clear_all(void)
{
    s_acc = s_cur = 0;
    s_frac = 0;
    s_op = 0;
    s_fresh = true;
    show(0);
    show_sub();
}

static void key_cb(lv_event_t *e)
{
    char k = (char)(intptr_t)lv_event_get_user_data(e);
    if (k >= '0' && k <= '9')      digit(k - '0');
    else if (k == '.')             { if (s_fresh) { s_cur = 0; s_fresh = false; } if (s_frac == 0) s_frac = 0.1; }
    else if (k == '=')             equals();
    else if (k == 'C')             clear_all();
    else                           op_key(k);
}

/* 화면을 지구본으로 보고 그 위에 격자를 그린다.
 *
 * 위젯을 사각형으로 늘어놓는 한 경계는 절대 곡선이 안 된다. 그래서
 * 판 전체를 캔버스에 직접 그린다. 화면을 정면에서 본 구(球)로 보고
 *   위도 φ = asin(dy/R),  경도 λ = asin(dx / (R·cosφ))
 * 로 칸을 나누면, 위도선(가로 경계)과 경선(세로 경계)이 둘 다 휜다.
 * 손잡이가 있는 아래쪽까지 판이 이어져 빈 데가 없다.
 *
 * 픽셀마다 삼각함수를 부르지 않는다 — 줄마다 경계 x 를 다섯 개만 구해
 * 그 사이를 채운다. 그래서 한 번 그리는 데 몇 ms 면 끝난다. */
#define R_PX      233.0f
#define COLS      4
#define ROWS      4
#define PHI_TOP   (-0.44f)      /* 위쪽 표시 구획이 끝나는 위도 (sin 값) */
#define PHI_BOT   ( 0.78f)      /* 격자가 끝나는 위도. 그 아래는 마지막 줄이 이어진다
                                 * — 손잡이 자리도 판의 연속으로 보이게 */

static lv_obj_t *s_canvas;
static void     *s_cbuf;
static lv_timer_t *s_free_t;   /* 캔버스 반납 예약 */

/* 칸 배치 — 스케치대로. 연산자는 오른쪽 열에 ÷ × − + */
static const char *KEY[ROWS][COLS] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "C", "0", "=", "+" },
};

static uint32_t key_color(char id)
{
    if (id == '=') return 0x2E6E9E;
    if (id == 'C') return 0x5A2E36;
    if (id == '/' || id == '*' || id == '-' || id == '+') return 0x1E3C56;
    return 0x212129;
}

/* 가로 경계도 휘게 한다. 구를 정면에서 보면 위도선은 직선이라,
 * 가운데가 아래로 처지도록 일부러 sag 를 준다 — 스케치의 그 모양이다. */
#define SAG   18.0f

static inline float sag_at(int x)
{
    float dxn = ((float)x - 233.0f) / R_PX;
    float v = 1.0f - dxn * dxn;
    return v <= 0 ? 0 : SAG * v;
}

/* (x,y) → 그 자리의 sin(위도). -1(위) ~ 1(아래) */
static inline float sin_phi_at(int x, int y)
{
    return ((float)y - 233.0f - sag_at(x)) / R_PX;
}

/* 줄 경계의 sin(위도). 표시 구획 아래를 네 줄로 나눈다 */
static float row_edge(int r)
{
    return PHI_TOP + (PHI_BOT - PHI_TOP) * (float)r / ROWS;
}

/* 그 줄에서 열 경계가 놓이는 x. 경도를 등분해서 구면에 되돌린다. */
static int col_edge(float cosphi, int c)
{
    float lam = (-1.0f + 2.0f * (float)c / COLS) * 1.2217f;   /* ±70도 */
    return (int)(233.0f + R_PX * cosphi * sinf(lam));
}

static void draw_panel(void)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(s_canvas);
    if (!db || !db->data) return;
    uint16_t *base = (uint16_t *)db->data;
    uint32_t  stride = db->header.stride / 2;
    if (stride < 466) return;

    uint16_t line = lv_color_to_u16(lv_color_hex(0x08080A));
    uint16_t band = lv_color_to_u16(lv_color_hex(0x0E0E12));

    for (int y = 0; y < 466; y++) {
        uint16_t *row = base + y * stride;
        float spr = ((float)y - 233.0f) / R_PX;      /* 원 모양은 그대로 */
        if (spr < -1.0f) spr = -1.0f;
        if (spr >  1.0f) spr =  1.0f;
        float cosphi = sqrtf(1.0f - spr * spr);
        int half = (int)(R_PX * cosphi);
        int x0 = 233 - half, x1 = 233 + half;

        /* 원 밖은 검정 */
        for (int x = 0; x < 466; x++) row[x] = 0;

        int e[COLS + 1];
        for (int c = 0; c <= COLS; c++) e[c] = col_edge(cosphi, c);
        e[0] = x0; e[COLS] = x1;                  /* 양끝은 원에 붙인다 */

        for (int c = 0; c < COLS; c++) {
            for (int x = e[c]; x < e[c + 1]; x++) {
                if (x < 0 || x >= 466) continue;

                /* 이 픽셀의 위도 — sag 때문에 가로 경계가 휜다 */
                float sp = sin_phi_at(x, y);
                if (sp < PHI_TOP) { row[x] = band; continue; }

                int r = 0;
                while (r < ROWS - 1 && sp >= row_edge(r + 1)) r++;

                bool onrow = false;
                for (int k = 1; k < ROWS; k++) {
                    if (fabsf(sp - row_edge(k)) * R_PX < 1.3f) { onrow = true; break; }
                }
                bool onedge = (c > 0 && x - e[c] < 2) || onrow;
                row[x] = onedge ? line
                                : lv_color_to_u16(lv_color_hex(key_color(KEY[r][c][0])));
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* 누른 자리가 어느 칸인지 — 그릴 때와 같은 식으로 되짚는다 */
static bool hit_cell(int x, int y, int *rr, int *cc)
{
    float dx = x - 233.0f, dy = y - 233.0f;
    if (dx * dx + dy * dy > R_PX * R_PX) return false;

    float sp = sin_phi_at(x, y);
    if (sp < PHI_TOP) return false;               /* 표시 구획 */
    float spr = dy / R_PX;
    float cosphi = sqrtf(1.0f - spr * spr);

    int r = 0;
    while (r < ROWS - 1 && sp >= row_edge(r + 1)) r++;

    int c = 0;
    while (c < COLS - 1 && x >= col_edge(cosphi, c + 1)) c++;
    *rr = r; *cc = c;
    return true;
}

static void panel_touch(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);

    int r, c;
    if (!hit_cell(p.x, p.y, &r, &c)) return;
    char id = KEY[r][c][0];

    if (id >= '0' && id <= '9')      digit(id - '0');
    else if (id == '.')              { if (s_fresh) { s_cur = 0; s_fresh = false; } if (s_frac == 0) s_frac = 0.1; }
    else if (id == '=')              equals();
    else if (id == 'C')              clear_all();
    else                             op_key(id);
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_CALC);
    /* 나가면서 걸어둔 반납 예약이 있으면 취소하고 그대로 쓴다 */
    if (s_free_t) { lv_timer_delete(s_free_t); s_free_t = NULL; }
    if (!s_cbuf) s_cbuf = port_big_alloc(LV_CANVAS_BUF_SIZE(466, 466, 16, LV_DRAW_BUF_ALIGN));

    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_cbuf, 466, 466, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    draw_panel();

    /* 글자는 칸 중심에 곧게. 기울이면 결과물이 지저분해진다. */
    for (int r = 0; r < ROWS; r++) {
        float sp = (row_edge(r) + row_edge(r + 1)) * 0.5f;
        float cosphi = sqrtf(1.0f - sp * sp);
        for (int c = 0; c < COLS; c++) {
            int x = (col_edge(cosphi, c) + col_edge(cosphi, c + 1)) / 2;
            int y = (int)(233.0f + R_PX * sp + sag_at(x));
            const char *t = KEY[r][c];

            /* ÷ 는 LVGL 기본 폰트(Montserrat)에 없다 — 쓰면 두부로 나온다.
             * 막대 하나와 점 두 개로 직접 그린다. */
            if (t[0] == '/') {
                lv_obj_t *bar = lv_obj_create(root);
                lv_obj_remove_style_all(bar);
                lv_obj_set_size(bar, 26, 3);
                lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_pos(bar, x - 13, y - 1);
                for (int d = 0; d < 2; d++) {
                    lv_obj_t *p2 = lv_obj_create(root);
                    lv_obj_remove_style_all(p2);
                    lv_obj_set_size(p2, 5, 5);
                    lv_obj_set_style_radius(p2, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_color(p2, lv_color_white(), 0);
                    lv_obj_set_style_bg_opa(p2, LV_OPA_COVER, 0);
                    lv_obj_set_pos(p2, x - 2, y + (d ? 7 : -11));
                }
                continue;
            }

            lv_obj_t *l = lv_label_create(root);
            lv_label_set_text(l, t[0] == '*' ? LV_SYMBOL_CLOSE : t);
            lv_obj_set_style_text_font(l, t[0] == '*' ? &lv_font_montserrat_20
                                                      : &lv_font_montserrat_26, 0);
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_update_layout(l);
            lv_obj_set_pos(l, x - lv_obj_get_width(l) / 2, y - lv_obj_get_height(l) / 2);
        }
    }

    s_disp = lv_label_create(root);
    lv_obj_set_style_text_font(s_disp, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_disp, lv_color_white(), 0);
    lv_obj_align(s_disp, LV_ALIGN_TOP_MID, 0, 44);

    s_sub = lv_label_create(root);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0xE0A33A), 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, -112, 52);

    lv_obj_t *pad = lv_obj_create(root);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 466, 466);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, panel_touch, LV_EVENT_CLICKED, NULL);

    clear_all();
}

/* ── 캔버스 반납 ─────────────────────────────────────────────
 * 466x466 RGB565 = 434KB 라 들고 있을 이유가 없는 크기다. 계산기는 잠깐
 * 쓰고 나가는 앱이니 나갈 때 돌려준다.
 *
 * 다만 leave() 는 화면이 페이드아웃하기 "전에" 불린다. 여기서 곧바로
 * 해제하면 사라지는 동안 캔버스가 해제된 메모리를 그린다(시뮬에서 바로
 * 세그폴트로 잡혔다). 그래서 애니메이션이 끝나고 화면이 실제로 지워진
 * 뒤에 반납한다. 그 사이에 다시 들어오면 예약을 취소하고 그대로 쓴다. */
static void free_cb(lv_timer_t *t)
{
    (void)t;
    s_free_t = NULL;
    if (s_cbuf) { port_big_free(s_cbuf); s_cbuf = NULL; }
}

static void leave(void)
{
    s_canvas = NULL;
    if (s_free_t) return;
    s_free_t = lv_timer_create(free_cb, 500, NULL);   /* 닫힘 애니메이션보다 길게 */
    lv_timer_set_repeat_count(s_free_t, 1);
}
static lv_color_t tint(void) { return lv_color_hex(0xE0A33A); }

const badge_app_t app_calc = {
    .name = "Calc", .art = &app_icon_calc, .icon = LV_SYMBOL_LIST, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
