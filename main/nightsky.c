/* The night sky. See nightsky.h for why this is drawn and not a picture. */
#include "nightsky.h"
#include <math.h>
#include <string.h>
#ifdef BADGE_SIM
#  include <stdlib.h>
#  include <stdio.h>
#  include <sys/time.h>
#  define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
#else
#  include "esp_heap_caps.h"
#endif

#define SCR   466
#define CX    233
#define CY    233
#define R_OUT 233

/* ── the picture ──────────────────────────────────────────────
 * 🚨 Drawn into a buffer once, not with LVGL shapes. Clouds need a colour that
 * changes a little at every pixel, and shapes cannot do that: a two-stop
 * gradient crosses five RGB565 blue steps over 466 rows and each crossing is a
 * hard line across the screen, and a glow built from circles shows the circles
 * as rings. Per-pixel with dithering has neither problem.
 *
 * 434 KB in PSRAM, worked out once and kept. The home screen is rebuilt on
 * every page turn and re-computing this each time would be seen. */
static uint16_t *s_px;

/* ── noise ────────────────────────────────────────────────────
 * Value noise at several scales, the same shape orb.c uses to bake planets. */
static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float vnoise(float x, float y)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    #define N(a,b) ((float)(hash2(a, b) & 0xFFFF) / 65535.0f)
    float n00 = N(xi, yi),     n10 = N(xi + 1, yi);
    float n01 = N(xi, yi + 1), n11 = N(xi + 1, yi + 1);
    #undef N
    return (n00 * (1 - u) + n10 * u) * (1 - v) + (n01 * (1 - u) + n11 * u) * v;
}
static float fbm(float x, float y, int oct)
{
    float sum = 0, amp = 0.5f, tot = 0;
    for (int i = 0; i < oct; i++) {
        sum += vnoise(x, y) * amp;
        tot += amp;
        x *= 2.03f; y *= 2.01f; amp *= 0.52f;
    }
    return sum / tot;
}

/* ── dithering ────────────────────────────────────────────────
 * 🚨 RGB565 has 32 levels of blue. A cloud that drifts from one level to the
 * next draws a visible edge exactly where it crosses. Shifting each pixel by a
 * fraction of one level first turns that edge into a scatter the eye reads as
 * a smooth ramp — the same trick tools/mkassets.py uses on its icons. */
static const uint8_t BAYER[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static inline uint16_t pack(float r, float g, float b, int x, int y)
{
    float t = (float)BAYER[y & 3][x & 3] / 16.0f - 0.5f;
    int ri = (int)(r + t * 8.0f);      /* one step of red   is 8 of 255 */
    int gi = (int)(g + t * 4.0f);      /* one step of green is 4        */
    int bi = (int)(b + t * 8.0f);
    if (ri < 0)   ri = 0;
    if (ri > 255) ri = 255;
    if (gi < 0)   gi = 0;
    if (gi > 255) gi = 255;
    if (bi < 0)   bi = 0;
    if (bi > 255) bi = 255;
    return (uint16_t)(((ri & 0xF8) << 8) | ((gi & 0xFC) << 3) | (bi >> 3));
}

/* ── painting ─────────────────────────────────────────────────
 * Three layers. A ground that is blue at the bottom and nearly black at the
 * top; clouds in three colours that only show where their noise is strong;
 * and the stars, which go in last so a cloud never covers one.
 *
 * 🚨 The clouds are worked out on a coarse grid and stretched over the screen,
 * not computed at every pixel. Per pixel it took 32 ms on a desktop, which on
 * this board is a second or two of the home screen simply not arriving. The
 * smallest thing in the picture is about ninety pixels across and the grid is
 * ten, so nothing is lost — and the dither still runs per pixel, which is what
 * actually decides whether the result bands. */
#define GRID 48                      /* 49x49 samples across 466 px */

static void clouds(float *gr, float *gg, float *gb)
{
    for (int j = 0; j <= GRID; j++) {
        float fy = (float)j / GRID;
        for (int i = 0; i <= GRID; i++) {
            float fx = (float)i / GRID;

            /* Ground. Deep blue low down, thinning to almost nothing at the
             * top — the way a sky looks toward a city and away from one. */
            float lift = fy * fy;               /* squared, so the top stays dark */
            float r = 8.0f  + 20.0f * lift;
            float g = 12.0f + 28.0f * lift;
            float b = 30.0f + 74.0f * lift;

            /* The violet cloud. Large and soft, thickest across the middle. */
            float c1 = fbm(fx * 2.6f + 1.7f, fy * 2.6f + 4.2f, 5);
            c1 = (c1 - 0.44f) / 0.56f;
            if (c1 > 0) {
                float k = c1 * c1 * 118.0f;
                r += k * 0.62f; g += k * 0.30f; b += k * 0.95f;
            }

            /* The cold one, running the other way, so the two cross rather
             * than sit on top of each other. */
            float c2 = fbm(fy * 3.4f + 9.1f, fx * 3.4f + 2.3f, 5);
            c2 = (c2 - 0.52f) / 0.48f;
            if (c2 > 0) {
                float k = c2 * c2 * 90.0f;
                r += k * 0.10f; g += k * 0.52f; b += k * 0.74f;
            }

            /* A third field, small and rare, in a colour neither of the other
             * two can make. Without it the sky is a blue wash with a purple
             * corner; with it there is something to find. */
            float c3 = fbm(fx * 5.2f + 21.4f, fy * 5.2f + 13.9f, 4);
            c3 = (c3 - 0.66f) / 0.34f;
            if (c3 > 0) {
                float k = c3 * c3 * 96.0f;
                r += k * 0.90f; g += k * 0.22f; b += k * 0.66f;   /* magenta */
            }

            /* A warm trace where the first two overlap, which is what keeps it
             * from reading as one flat blue wash. */
            if (c1 > 0.35f && c2 > 0.30f) {
                float k = (c1 - 0.35f) * (c2 - 0.30f) * 170.0f;
                r += k * 1.00f; g += k * 0.46f; b += k * 0.30f;
            }

            int o = j * (GRID + 1) + i;
            gr[o] = r; gg[o] = g; gb[o] = b;
        }
    }
}

static void paint(void)
{
    const int N = (GRID + 1) * (GRID + 1);
    float *grid = heap_caps_malloc((size_t)N * 3 * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!grid) { memset(s_px, 0, (size_t)SCR * SCR * 2); return; }
    float *gr = grid, *gg = grid + N, *gb = grid + 2 * N;
    clouds(gr, gg, gb);

    const float step = (float)GRID / SCR;
    for (int y = 0; y < SCR; y++) {
        float gy = y * step;
        int   j  = (int)gy; if (j >= GRID) j = GRID - 1;
        float ty = gy - j;
        int   row0 = j * (GRID + 1), row1 = row0 + (GRID + 1);
        int   dy = y - CY;

        for (int x = 0; x < SCR; x++) {
            int dx = x - CX;
            int d2 = dx * dx + dy * dy;
            if (d2 > R_OUT * R_OUT) { s_px[y * SCR + x] = 0; continue; }

            float gx = x * step;
            int   i  = (int)gx; if (i >= GRID) i = GRID - 1;
            float tx = gx - i;

            float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
            float w01 = (1 - tx) * ty,       w11 = tx * ty;
            int   a = row0 + i, b_ = row1 + i;

            float r = gr[a] * w00 + gr[a + 1] * w10 + gr[b_] * w01 + gr[b_ + 1] * w11;
            float g = gg[a] * w00 + gg[a + 1] * w10 + gg[b_] * w01 + gg[b_ + 1] * w11;
            float b = gb[a] * w00 + gb[a + 1] * w10 + gb[b_] * w01 + gb[b_ + 1] * w11;

            /* The rim of a round screen catches the eye. Sink it slightly so
             * the disc reads as a dome rather than a sticker. Only the outer
             * ring needs the square root. */
            if (d2 > (int)(0.80f * 0.80f * R_OUT * R_OUT)) {
                float rad = sqrtf((float)d2) / R_OUT;
                float k = 1.0f - (rad - 0.80f) / 0.20f * 0.55f;
                r *= k; g *= k; b *= k;
            }

            s_px[y * SCR + x] = pack(r, g, b, x, y);
        }
    }
    heap_caps_free(grid);
}

/* ── the stars ────────────────────────────────────────────────
 * Positions come from a hash rather than a table, so several hundred stars
 * cost no flash at all. The same seed gives the same sky every boot — a
 * wallpaper that rearranges itself when you turn the page would be unsettling. */
#define STARS      260
#define HAZE_STARS 170     /* the extra ones crowding the milky way */

/* The milky way runs corner to corner. A band, not a line. */
#define BAND_DX    0.64f
#define BAND_DY   -0.77f

static void put(int x, int y, int r, int g, int b, float a)
{
    if (x < 0 || y < 0 || x >= SCR || y >= SCR) return;
    int dx = x - CX, dy = y - CY;
    if (dx * dx + dy * dy > R_OUT * R_OUT) return;
    uint16_t o = s_px[y * SCR + x];
    float orr = (float)((o >> 11) & 0x1F) * 8.23f;
    float og  = (float)((o >>  5) & 0x3F) * 4.05f;
    float ob  = (float)( o        & 0x1F) * 8.23f;
    s_px[y * SCR + x] = pack(orr + (r - orr) * a,
                             og  + (g - og)  * a,
                             ob  + (b - ob)  * a, x, y);
}

static void sprinkle(void)
{
    for (int i = 0; i < STARS + HAZE_STARS; i++) {
        uint32_t h  = hash2(i * 7919, 104729);
        uint32_t h2 = hash2((int)h, 31337);

        int x, y;
        if (i < STARS) {
            x = (int)(h  % SCR);
            y = (int)(h2 % SCR);
        } else {
            float along  = ((float)(h  % 1000) / 1000.0f - 0.5f) * 540.0f;
            float across = ((float)(h2 % 1000) / 1000.0f - 0.5f) * 112.0f;
            across *= (float)(h2 % 7 + 1) / 7.0f;   /* crowded at the spine */
            x = CX + (int)(BAND_DX * along - BAND_DY * across);
            y = CY + (int)(BAND_DY * along + BAND_DX * across);
        }

        uint32_t h3 = hash2((int)h2, 6971);
        int      q  = (int)(h3 % 100);

        /* Mostly faint. A sky of equally bright dots reads as noise, not sky. */
        float a; int big;
        if      (q < 64) { a = 0.16f + (float)((h3 >> 8) % 24) / 200.0f; big = 0; }
        else if (q < 91) { a = 0.42f + (float)((h3 >> 8) % 28) / 140.0f; big = 0; }
        else             { a = 0.80f + (float)((h3 >> 8) % 20) / 100.0f; big = 1; }
        if (i >= STARS) a *= 0.55f;                 /* the band's own are dimmer */
        if (a > 1.0f) a = 1.0f;

        /* A few warm, a few cool, the rest white. Real skies are not monochrome. */
        int r = 255, g = 255, b = 255;
        switch ((h3 >> 16) % 12) {
        case 0: case 1: r = 255; g = 226; b = 190; break;
        case 2:         r = 255; g = 206; b = 156; break;
        case 3: case 4: r = 206; g = 224; b = 255; break;
        default: break;
        }

        put(x, y, r, g, b, a);
        if (big) {
            put(x + 1, y,     r, g, b, a * 0.85f);
            put(x,     y + 1, r, g, b, a * 0.85f);
            put(x + 1, y + 1, r, g, b, a * 0.70f);
            /* The brightest get a cross of light, which is what sells them */
            if ((h3 >> 24) % 3 == 0) {
                for (int k = 2; k <= 4; k++) {
                    float f = a * (0.34f - (float)k * 0.06f);
                    put(x - k, y, r, g, b, f); put(x + 1 + k, y, r, g, b, f);
                    put(x, y - k, r, g, b, f); put(x, y + 1 + k, r, g, b, f);
                }
            }
        }
    }
}

lv_obj_t *nightsky_create(lv_obj_t *parent)
{
    if (!s_px) {
        s_px = heap_caps_malloc((size_t)SCR * SCR * 2, MALLOC_CAP_SPIRAM);
        if (s_px) {
#ifdef BADGE_SIM
            struct timeval a, b; gettimeofday(&a, NULL);
#endif
            paint(); sprinkle();
#ifdef BADGE_SIM
            gettimeofday(&b, NULL);
            fprintf(stderr, "[nightsky] paint %.1f ms\n",
                    (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0);
#endif
        }
    }

    lv_obj_t *o = lv_canvas_create(parent);
    if (s_px) lv_canvas_set_buffer(o, s_px, SCR, SCR, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(o);
    /* 🚨 A canvas is clickable by default. Left that way the wallpaper is the
     * pressed object for every swipe on the bare background, the screen never
     * sees the gesture, and the home pages stop turning. */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
