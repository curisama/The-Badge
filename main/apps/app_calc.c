/* The calculator. Built because reaching for the phone and hunting for its
 * calculator is a nuisance.
 *
 * A 4x4 grid on a round screen loses its four corners. The digits stack in
 * three columns down the middle and the operators sit on the left and right
 * flanks of the circle — on a round screen that is where the room is. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>
#include <math.h>
#define DEG2RAD 0.0174533f

static lv_obj_t *s_disp, *s_sub;

static double s_acc;        /* the running value */
static double s_cur;        /* what is being typed now */
static double s_frac;       /* decimal place (0 while typing the integer part) */
static char   s_op;         /* the operator waiting */
static bool   s_fresh;      /* does the next digit start a new number? */

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
    case '/': return b == 0 ? 0 : a / b;   /* divide by zero gives 0 — better than dying */
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

/* The screen is treated as a globe and the grid is drawn on it.
 *
 * Laying rectangular widgets out can never curve the boundaries, so the whole
 * panel is drawn straight onto a canvas. Take the screen as a sphere seen
 * head-on and divide the cells by
 *   latitude  φ = asin(dy/R),  longitude λ = asin(dx / (R·cosφ))
 * and both the parallels (horizontal edges) and the meridians (vertical
 * edges) bend. The panel runs all the way down past the handle, so there is
 * no gap anywhere.
 *
 * Trigonometry is not called per pixel — five boundary x values per row are
 * enough, and the spans between them are filled. One draw takes a few ms. */
#define R_PX      233.0f
#define COLS      4
#define ROWS      4
#define PHI_TOP   (-0.44f)      /* latitude where the display area ends (as a sin) */
#define PHI_BOT   ( 0.78f)      /* latitude where the grid ends. Below it the last row
                                 * continues — so the handle area still reads as panel */

static lv_obj_t *s_canvas;
static void     *s_cbuf;
static lv_timer_t *s_free_t;   /* the canvas hand-back, booked ahead */

/* The cells — as sketched. Operators run down the right column: ÷ × − + */
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

/* The horizontal edges bend too. Parallels on a sphere seen head-on are
 * straight, so a deliberate sag drops their middles — that is the shape in
 * the sketch. */
#define SAG   18.0f

static inline float sag_at(int x)
{
    float dxn = ((float)x - 233.0f) / R_PX;
    float v = 1.0f - dxn * dxn;
    return v <= 0 ? 0 : SAG * v;
}

/* (x,y) → sin(latitude) there. -1 (top) to 1 (bottom) */
static inline float sin_phi_at(int x, int y)
{
    return ((float)y - 233.0f - sag_at(x)) / R_PX;
}

/* sin(latitude) of a row edge. Below the display area it splits into four rows */
static float row_edge(int r)
{
    return PHI_TOP + (PHI_BOT - PHI_TOP) * (float)r / ROWS;
}

/* Where a column edge falls on that row. Longitude is divided evenly and put back on the sphere. */
static int col_edge(float cosphi, int c)
{
    float lam = (-1.0f + 2.0f * (float)c / COLS) * 1.2217f;   /* ±70 degrees */
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
        float spr = ((float)y - 233.0f) / R_PX;      /* the circle keeps its shape */
        if (spr < -1.0f) spr = -1.0f;
        if (spr >  1.0f) spr =  1.0f;
        float cosphi = sqrtf(1.0f - spr * spr);
        int half = (int)(R_PX * cosphi);
        int x0 = 233 - half, x1 = 233 + half;

        /* Outside the circle is black */
        for (int x = 0; x < 466; x++) row[x] = 0;

        int e[COLS + 1];
        for (int c = 0; c <= COLS; c++) e[c] = col_edge(cosphi, c);
        e[0] = x0; e[COLS] = x1;                  /* the two ends meet the circle */

        for (int c = 0; c < COLS; c++) {
            for (int x = e[c]; x < e[c + 1]; x++) {
                if (x < 0 || x >= 466) continue;

                /* This pixel's latitude — the sag is what bends the horizontal edges */
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

/* Which cell was pressed — retraced the same way it was drawn */
static bool hit_cell(int x, int y, int *rr, int *cc)
{
    float dx = x - 233.0f, dy = y - 233.0f;
    if (dx * dx + dy * dy > R_PX * R_PX) return false;

    float sp = sin_phi_at(x, y);
    if (sp < PHI_TOP) return false;               /* the display area */
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
    /* If a hand-back was booked on the way out, cancel it and use the canvas as it is */
    if (s_free_t) { lv_timer_delete(s_free_t); s_free_t = NULL; }
    if (!s_cbuf) s_cbuf = port_big_alloc(LV_CANVAS_BUF_SIZE(466, 466, 16, LV_DRAW_BUF_ALIGN));

    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_cbuf, 466, 466, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    draw_panel();

    /* Text sits upright in the centre of its cell. Tilting it only looks messy. */
    for (int r = 0; r < ROWS; r++) {
        float sp = (row_edge(r) + row_edge(r + 1)) * 0.5f;
        float cosphi = sqrtf(1.0f - sp * sp);
        for (int c = 0; c < COLS; c++) {
            int x = (col_edge(cosphi, c) + col_edge(cosphi, c + 1)) / 2;
            int y = (int)(233.0f + R_PX * sp + sag_at(x));
            const char *t = KEY[r][c];

            /* ÷ is not in LVGL's default font (Montserrat) — using it gives a
             * tofu box. It is drawn by hand from one bar and two dots. */
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

/* ── handing the canvas back ─────────────────────────────────
 * 466x466 RGB565 is 434 KB, too much to hold on to for no reason. The
 * calculator is used briefly and left, so it is given back on the way out.
 *
 * But leave() runs *before* the screen fades out. Freeing it here has the
 * canvas drawing freed memory while it disappears (the simulator caught it as
 * a segfault straight away). So it is handed back after the animation ends
 * and the screen is actually gone. Coming back in before then cancels the
 * booking and keeps the canvas. */
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
    s_free_t = lv_timer_create(free_cb, 500, NULL);   /* longer than the closing animation */
    lv_timer_set_repeat_count(s_free_t, 1);
}
static lv_color_t tint(void) { return lv_color_hex(0xE0A33A); }

const badge_app_t app_calc = {
    .name = "Calc", .art = &app_icon_calc, .icon = LV_SYMBOL_LIST, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
