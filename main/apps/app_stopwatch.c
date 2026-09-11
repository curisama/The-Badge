/* The stopwatch. It lives as one page of the clock app (swiped vertically in
 * the tileview).
 *
 * Where the timer is "watch the ring shrink", here the digits are the whole
 * point — the eye is on the number because measuring is what it is for. So
 * there is no ring, just large text.
 *
 * 🚨 The screen must not go off while it runs (raised 09-10). But it is held
 * **only while running**. Blocking it outright would keep the screen alive on
 * a desk and burn the battery — the water app made the same call. It lets go
 * the instant it stops.
 *
 * 🚨 The digits must not go into one label (raised 09-10). Montserrat is a
 * proportional font, so 1 and 8 are different widths. In a single centred
 * label the total width changes every hundredth of a second and the number
 * shivers left and right. Each character gets its own cell, and the cell
 * width is fixed at the widest digit — the positions hold still and only the
 * glyphs change. row_t below does that.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

/* ── the fixed-width digit row ──────────────────────────────── */
#define SLOT_N 8

typedef struct {
    lv_obj_t *slot[SLOT_N];
    char      ch[SLOT_N];      /* the character in each cell — untouched if unchanged */
    int16_t   dw, sw;          /* digit cell width, separator (: .) cell width */
    int16_t   dx, dy;
    uint32_t  sig;             /* re-laid out only when the cells actually moved */
    uint32_t  col;
} row_t;

static void row_measure(row_t *r, const lv_font_t *f, int pad)
{
    /* Cell widths are measured from the font itself. Digits use the widest of
     * them — that way no digit can shake the cell. */
    uint16_t dw = 0;
    for (uint32_t c = '0'; c <= '9'; c++) {
        uint16_t w = lv_font_get_glyph_width(f, c, 0);
        if (w > dw) dw = w;
    }
    uint16_t sw = lv_font_get_glyph_width(f, ':', 0);
    uint16_t pw = lv_font_get_glyph_width(f, '.', 0);
    if (pw > sw) sw = pw;
    /* pad is the breathing room between cells. Fitting the glyph exactly makes
     * digits crowd each other in the large font — the bigger the row, the
     * more generous. */
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

/* Up to eight characters. A digit gets a wide cell, a : or . a narrow one. */
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

/* The width of an eight-character row. Used to put "lap" to its left. */
static int row_width8(const row_t *r) { return 6 * r->dw + 2 * r->sw; }

/* ── state ──────────────────────────────────────────────────── */
static row_t       s_bigrow, s_laprow;
static lv_obj_t   *s_lapword, *s_hint;
static lv_timer_t *s_tick;

static bool     s_run;
static uint32_t s_base_ms;      /* when it started running */
static uint32_t s_acc_ms;       /* what piled up while it was stopped */
static uint32_t s_lap_ms;       /* where the last lap was taken */
static uint32_t s_press_ms;
static bool     s_long_done;

static uint32_t elapsed(void)
{
    /* 🚨 lv_tick_get() is 32-bit milliseconds and wraps every 49 days.
     * Subtraction is still correct across the wrap (unsigned arithmetic), so
     * this expression holds — just never compare the values directly. */
    return s_acc_ms + (s_run ? (lv_tick_get() - s_base_ms) : 0);
}

static void paint(void)
{
    uint32_t ms = elapsed();
    uint32_t cs = (ms / 10) % 100;
    uint32_t ss = (ms / 1000) % 60;
    uint32_t mm = (ms / 60000) % 60;
    uint32_t hh =  ms / 3600000;
    if (hh > 99) hh = 99;          /* never spill past the eight cells */

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
    /* 🔋 Not running, no reason to repaint. The same goes for a screen that is
     * off — time still passes (elapsed reads the clock, so nothing drifts). */
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
    /* 🚨 Held only while running. Several things can hold at once, so it takes
     * only its own — stopping the stopwatch while a timer alarm is ringing
     * does not release the alarm's hold. */
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
            s_lap_ms = elapsed();          /* long press while running = lap */
        } else {
            s_acc_ms = 0; s_lap_ms = 0;    /* long press while stopped = reset */
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

    /* This screen exists to be read, so the digits are as large as they go —
     * 48 is the biggest font in this firmware. Cells are spread 6 px apart so
     * they do not crowd. */
    row_make(&s_bigrow, root, &lv_font_montserrat_48, 0, -26, 6, 0x7FB0FF);

    /* The lap puts "lap" on the left with the digits to its right. A digit row
     * is always eight cells, so the width is known and the two can be placed
     * ahead of time — "lap" does not shift when the lap changes. */
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

    /* 🚨 Inside a tileview, a vertical swipe is taken as turning the page. Only presses here. */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, press_cb, LV_EVENT_RELEASED, NULL);

    /* Hundredths have to look like they are flowing, so this cannot be slower.
     * It only does real work while running (tick above), so the cost while
     * stopped is zero. */
    s_tick = lv_timer_create(tick, 47, NULL);
    paint();
}

void stopwatch_free(void)
{
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    /* 🚨 Let go on the way out. Otherwise the screen never turns off, even with the clock app closed. */
    launcher_keep_awake_by(AWAKE_STOP, false);
    s_run = false;
    /* 🚨 The pieces went with the tile already. Only the pointers are cleared —
     * left behind, the next open would touch something that is gone. */
    memset(&s_bigrow, 0, sizeof s_bigrow);
    memset(&s_laprow, 0, sizeof s_laprow);
    s_lapword = s_hint = NULL;
}
