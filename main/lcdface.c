/* True LCD face. See lcdface.h for why this is shapes and not a picture. */
#include "lcdface.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "port.h"

#define CX 233
#define CY 233

/* ── geometry ─────────────────────────────────────────────────
 * Seven-segment cells, taken from the design's clip paths. Large cells are
 * 56x98, small ones 34x58. Order is a b c d e f g, matching GLYPH below. */
typedef struct { int x, y, w, h; } seg_rect_t;

static const seg_rect_t SEG_BIG[7] = {
    { 8,  0, 40, 10},  /* a  top          */
    {46,  6, 10, 40},  /* b  upper right  */
    {46, 52, 10, 40},  /* c  lower right  */
    { 8, 88, 40, 10},  /* d  bottom       */
    { 0, 52, 10, 40},  /* e  lower left   */
    { 0,  6, 10, 40},  /* f  upper left   */
    { 8, 44, 40, 10},  /* g  middle       */
};
static const seg_rect_t SEG_SMALL[7] = {
    { 6,  0, 22, 7}, {27,  4, 7, 23}, {27, 31, 7, 23},
    { 6, 51, 22, 7}, { 0, 31, 7, 23}, { 0,  4, 7, 23}, { 6, 25, 22, 7},
};
#define BIG_W   56
#define BIG_H   98
#define SMALL_W 34
#define SMALL_H 58

static const uint8_t GLYPH[10] = {
    0x3F, /* 0  a b c d e f   */  0x06, /* 1  b c           */
    0x5B, /* 2  a b d e g     */  0x4F, /* 3  a b c d g     */
    0x66, /* 4  b c f g       */  0x6D, /* 5  a c d f g     */
    0x7D, /* 6  a c d e f g   */  0x07, /* 7  a b c         */
    0x7F, /* 8  every one     */  0x6F, /* 9  a b c d f g   */
};

/* ── colours ──────────────────────────────────────────────────
 * The panel is a grey-green TN sheet with near-black segments. An unlit
 * segment is not invisible — that faint ghost is what makes a real LCD read
 * as one, so it is drawn at low opacity rather than skipped. */
#define INK_ACTIVE  0x121A10
#define INK_AOD     0xB4DEBE
#define ON_ACTIVE   230         /* 0.90 */
#define OFF_ACTIVE  19          /* 0.075 */
#define ON_AOD      184         /* 0.72 */
#define OFF_AOD     14          /* 0.055 */
#define LAB_ACTIVE  184
#define LAB_AOD     102

/* ── one object per digit, not one per segment ────────────────
 * 🚨 The obvious build — an lv_obj for each of the seven segments — works on
 * a plain screen and hangs the Clock app. That app is a tileview, and past
 * roughly seventy children in a tile lv_timer_handler() stops returning: the
 * board spins at 100% CPU with a frozen screen. Four big digits (28 objects)
 * were fine; adding the bottom row (76 total) was not.
 *
 * Drawing the segments in LV_EVENT_DRAW_MAIN gets that down to ten objects,
 * and is less work per frame besides — the same reason the water app draws
 * its own band rather than handing LVGL hundreds of children. */
typedef struct {
    lv_obj_t *obj;
    uint8_t   bits;      /* which segments are lit */
    bool      big;
    bool      aod;
    int8_t    shown;     /* -1 = blank */
} cell_t;

struct lcdface_s {
    bool      aod;
    lv_obj_t *root;
    cell_t    hh[2], mm[2];         /* time                     */
    cell_t    ss[2], bt[2], up[2];  /* seconds, battery, uptime */
    lv_obj_t *head;                 /* "THU SEP 11"             */
    int       last_min, last_sec, last_day;
};

static uint32_t ink(const lcdface_t *f)     { return f->aod ? INK_AOD : INK_ACTIVE; }
static uint8_t  on_opa(const lcdface_t *f)  { return f->aod ? ON_AOD  : ON_ACTIVE; }

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void cell_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    cell_t *c = lv_obj_get_user_data(o);
    if (!c) return;
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t box;
    lv_obj_get_coords(o, &box);

    const seg_rect_t *r = c->big ? SEG_BIG : SEG_SMALL;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 2;
    d.bg_color = lv_color_hex(c->aod ? INK_AOD : INK_ACTIVE);

    for (int i = 0; i < 7; i++) {
        d.bg_opa = (c->bits & (1u << i)) ? (c->aod ? ON_AOD : ON_ACTIVE)
                                         : (c->aod ? OFF_AOD : OFF_ACTIVE);
        lv_area_t a = { .x1 = box.x1 + r[i].x,
                        .y1 = box.y1 + r[i].y,
                        .x2 = box.x1 + r[i].x + r[i].w - 1,
                        .y2 = box.y1 + r[i].y + r[i].h - 1 };
        lv_draw_rect(layer, &d, &a);
    }
}

static void cell_build(lcdface_t *f, cell_t *c, int x, int y, bool big)
{
    c->big = big;
    c->aod = f->aod;
    c->bits = 0;
    c->shown = -1;
    c->obj = plain(f->root);
    lv_obj_set_pos(c->obj, x, y);
    lv_obj_set_size(c->obj, big ? BIG_W : SMALL_W, big ? BIG_H : SMALL_H);
    lv_obj_set_user_data(c->obj, c);
    lv_obj_add_event_cb(c->obj, cell_draw, LV_EVENT_DRAW_MAIN, NULL);
}

static void cell_set(cell_t *c, int digit)
{
    if (!c->obj || c->shown == digit) return;
    c->shown = (int8_t)digit;
    c->bits = (digit >= 0 && digit <= 9) ? GLYPH[digit] : 0;
    lv_obj_invalidate(c->obj);
}

static void pair_set(cell_t *c, int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    cell_set(&c[0], v / 10);
    cell_set(&c[1], v % 10);
}

/* Labels get a fixed box and centre their own text inside it.
 *
 * 🚨 Letting the label size itself and then measuring it needs
 * lv_obj_update_layout(), which is not safe to call while building children
 * of a tileview. A fixed box needs no measuring. */
#define LAB_BOX 240

static lv_obj_t *label(lcdface_t *f, const char *txt, const lv_font_t *fnt,
                       int cx, int cy, uint8_t opa)
{
    lv_obj_t *l = lv_label_create(f->root);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, fnt, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(ink(f)), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_width(l, LAB_BOX);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, cx - LAB_BOX / 2, cy - fnt->line_height / 2);
    return l;
}

/* ── layout ───────────────────────────────────────────────────
 * Rows are stacked and the stack is centred, so changing one row's height
 * moves what is below it and nothing needs re-measuring by hand.
 *
 * The circle is the constraint. The widest row is the time at 315 px, which
 * needs |dy| <= 171 to fit; it sits well inside that. The bottom digit row is
 * the one to watch if anything grows. */
#define BLOCK_W   300
#define TIME_GAP  16
#define SMALL_GAP 9
#define H_HEAD    26
#define H_BOTLAB  18

static void build(lcdface_t *f)
{
    const int tcell = BIG_W + TIME_GAP;         /* 72 */
    const int ccell = 11 + TIME_GAP;            /* 27, the colon column */
    const int scell = SMALL_W + SMALL_GAP;      /* 43 */

    const int total = H_HEAD + 14 + BIG_H + 18 + H_BOTLAB + SMALL_H;
    const int y = (466 - total) / 2;
    const int y_head = y;
    const int y_time = y + H_HEAD + 14;
    const int y_blab = y_time + BIG_H + 18;
    const int y_bdig = y_blab + H_BOTLAB;

    int tx = CX - (tcell * 4 + ccell) / 2 + TIME_GAP / 2;
    cell_build(f, &f->hh[0], tx,                     y_time, true);
    cell_build(f, &f->hh[1], tx + tcell,             y_time, true);
    cell_build(f, &f->mm[0], tx + tcell * 2 + ccell, y_time, true);
    cell_build(f, &f->mm[1], tx + tcell * 3 + ccell, y_time, true);

    int colon_x = tx + tcell * 2 + TIME_GAP / 2 - 3;
    int colon_y = y_time + (BIG_H - (11 * 2 + 24)) / 2;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = plain(f->root);
        lv_obj_set_pos(d, colon_x, colon_y + i * 35);
        lv_obj_set_size(d, 11, 11);
        lv_obj_set_style_radius(d, 2, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(ink(f)), 0);
        lv_obj_set_style_bg_opa(d, on_opa(f), 0);
    }

    f->head = label(f, "--- --- --", &lv_font_montserrat_24,
                    CX, y_head + H_HEAD / 2, f->aod ? LAB_AOD : LAB_ACTIVE);

    /* The source design had temperature, humidity and rain here. This board
     * has none of those, so the row shows what it does know. */
    static const char *LAB[3] = { "SEC", "BAT", "UP" };
    cell_t *col[3] = { f->ss, f->bt, f->up };
    const int colw = BLOCK_W / 3;
    const int x0 = CX - BLOCK_W / 2;
    for (int i = 0; i < 3; i++) {
        int ccx = x0 + i * colw + colw / 2;
        label(f, LAB[i], &lv_font_montserrat_16, ccx, y_blab + H_BOTLAB / 2,
              f->aod ? LAB_AOD : LAB_ACTIVE);
        cell_build(f, &col[i][0], ccx - scell + SMALL_GAP / 2, y_bdig, false);
        cell_build(f, &col[i][1], ccx + SMALL_GAP / 2,         y_bdig, false);
    }

    /* The source had a diver's depth rating etched here. This board gets
     * what it actually is. */
    if (!f->aod)
        label(f, "ESP32-S3 . 466", &lv_font_montserrat_14, CX, 466 - 54, 87);
}

lcdface_t *lcdface_create(lv_obj_t *parent, bool aod)
{
    lcdface_t *f = lv_malloc(sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->aod = aod;
    f->last_min = f->last_sec = f->last_day = -1;

    f->root = plain(parent);
    lv_obj_set_size(f->root, 466, 466);
    lv_obj_set_pos(f->root, 0, 0);
    lv_obj_set_style_radius(f->root, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, 0);
    if (aod) {
        lv_obj_set_style_bg_color(f->root, lv_color_black(), 0);
    } else {
        /* The sheet is not flat — it is lighter at the edges than across the
         * middle. One gradient gets most of that. */
        lv_obj_set_style_bg_color(f->root, lv_color_hex(0xA7B494), 0);
        lv_obj_set_style_bg_grad_color(f->root, lv_color_hex(0x93A37E), 0);
        lv_obj_set_style_bg_grad_dir(f->root, LV_GRAD_DIR_VER, 0);
    }

    build(f);
    lcdface_update(f);
    return f;
}

void lcdface_update(lcdface_t *f)
{
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    if (tm.tm_min != f->last_min) {
        f->last_min = tm.tm_min;
        pair_set(f->hh, tm.tm_hour);
        pair_set(f->mm, tm.tm_min);
    }
    if (tm.tm_sec != f->last_sec) {
        f->last_sec = tm.tm_sec;
        pair_set(f->ss, tm.tm_sec);

        int pct = port_battery_percent();
        pair_set(f->bt, pct < 0 ? 0 : pct);

        /* Uptime in hours. LVGL's tick is milliseconds since boot and wraps
         * at 49 days; the modulo makes that harmless rather than wrong. */
        pair_set(f->up, (int)((lv_tick_get() / 3600000u) % 100u));
    }
    if (tm.tm_mday != f->last_day) {
        f->last_day = tm.tm_mday;
        static const char *WD[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
        static const char *MO[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        char buf[20];
        snprintf(buf, sizeof buf, "%s  %s %02d",
                 WD[tm.tm_wday % 7], MO[tm.tm_mon % 12], tm.tm_mday);
        if (f->head) lv_label_set_text(f->head, buf);
    }
}

void lcdface_destroy(lcdface_t *f)
{
    if (f) lv_free(f);
}
