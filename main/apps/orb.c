/* Moon and Earth — a sphere you spin with a finger.
 *
 * It does not draw a sphere in 3D. There is one equirectangular texture
 * (across = longitude, down = latitude), and for each pixel on screen it
 * works backwards to "where on that texture is this point?". That mapping
 * does not depend on the attitude, so it is computed once at startup into
 * PSRAM, and rotating is just adding an offset along the horizontal — no
 * multiplies and no trigonometry per frame.
 *
 * 🚨 The canvas lives in PSRAM (466x466x2 = 434 KB). That is fine here. The
 * 13,943 failed draws in an earlier version happened because the buffer LVGL
 * *pushes to the panel* was in PSRAM and could not be DMA'd. A canvas is not
 * that buffer — it is source pixels the CPU reads — so PSRAM is fine. The
 * push buffer is still sixteen lines of internal RAM.
 *
 * 🚨 Power. Redrawing the whole screen continuously is the most expensive
 * thing this device can do (a static screen alone costs 7.3x the display
 * being off). So it only draws while it is turning — let go and friction
 * stops it, and once stopped it draws no pixels at all.
 *
 * 🚨 The texture here is generated in code. To use a real photograph, bake
 * one with tools/make-orb-texture.py. */
#include "app.h"
#include "port.h"
#include "orb.h"
#include <math.h>
#include <string.h>
#ifdef BADGE_SIM
/* The simulator has neither PSRAM nor esp_timer. Stand-ins under the same names. */
#  include <stdlib.h>
#  include <stdio.h>
#  include <sys/time.h>
#  define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
static int64_t esp_timer_get_time(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
#  define ESP_LOGE(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#  define ESP_LOGI(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#  include "esp_heap_caps.h"
#  include "esp_timer.h"
#  include "esp_log.h"
#endif

#define CX      233
#define CY      233
#define ORB_R   206              /* radius of the sphere */
#define TEX_W   512              /* a power of two, so longitude wraps with & */
#define TEX_H   256

/* 🚨 To use a real photograph, bake it with tools/make-orb-texture.py.
 * When one is baked in it is read straight from flash and the 260 KB of
 * PSRAM is not needed. Without one, the code below generates a texture (the
 * Moon is convincing; the Earth is not an Earth). */
#if __has_include("assets/orb_tex_moon.h")
#  include "assets/orb_tex_moon.h"
#  define HAVE_MOON_TEX 1
#endif
#if __has_include("assets/orb_tex_earth.h")
#  include "assets/orb_tex_earth.h"
#  define HAVE_EARTH_TEX 1
#endif
#if __has_include("assets/orb_tex_sun.h")
#  include "assets/orb_tex_sun.h"
#  define HAVE_SUN_TEX 1
#endif
#if __has_include("assets/orb_tex_jupiter.h")
#  include "assets/orb_tex_jupiter.h"
#  define HAVE_JUPITER_TEX 1
#endif

const char *const ORB_NAME[ORB_N] = { "Moon", "Earth", "Sun", "Jupiter" };

static lv_obj_t   *s_canvas;
static lv_timer_t *s_loop;
static uint16_t   *s_fb;         /* the canvas pixels (PSRAM) */
static const uint16_t *s_tex;    /* RGB565 texture — either flash or the one below */
/* 🚨 These two must not be one variable. "I allocated it and must free it"
 * and "there is nothing baked in, so generate it" are different statements,
 * and combining them while moving the texture into PSRAM meant the generated
 * image was written over the real photograph that had just been copied there
 * — the Sun and Jupiter both came out as Earth. */
static uint16_t   *s_tex_owned;  /* I allocated this and must free it on the way out */
static bool        s_tex_bake;   /* nothing is baked in — generate one */
/* 🚨 Rotating horizontally only adds to the longitude and leaves the table
 * alone. Vertical is different: tilt the axis and every pixel's latitude and
 * longitude change together, which would mean solving 145,000 inverse
 * trigonometric functions per frame (hundreds of cycles each, so 0.4 s a
 * frame — impossible).
 *
 * Instead it is solved exactly on a grid every 8 pixels and interpolated
 * between. On a sphere that in-between is smooth enough not to show. The
 * grid is 2,916 points, so 145,000 solves become 2,916. And the grid depends
 * only on the vertical tilt, so spinning horizontally never recomputes it. */
/* Solved exactly per pixel. Solving on a coarse grid and interpolating was
 * tried: at the top and bottom of the circle latitude changes by a lot in a
 * single pixel, and it smeared badly in the vertical. A finer grid does not
 * win either, because at the edge the gradient is effectively infinite.
 *
 * So the inverse trigonometry is replaced with cheap approximations instead.
 * The standard library versions are hundreds of cycles, which will not do
 * 145,000 times, but atan2 as a polynomial is about fifteen operations and
 * asin as a table lookup with interpolation is a handful.
 *
 * The only thing recomputed per pixel is z (depth into the sphere), so that
 * is precomputed. */
static int16_t    *s_lut_z;      /* per-pixel z on the sphere (32767 = 1.0) */
static float      *s_col_x;      /* per-column x (-1..1) */
static float      *s_asin_tab;   /* asin table (1025 entries) */
static uint8_t    *s_lut_shade;  /* per-pixel brightness 0..255 — independent of tilt */

/* atan2 approximation — error below 1e-5. One divide and a few multiplies. */
static inline float atan2_fast(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float d = (ax > ay ? ax : ay) + 1e-12f;
    float a = (ax > ay ? ay : ax) / d;
    float ss = a * a;
    float r = ((-0.0464964749f * ss + 0.15931422f) * ss - 0.327622764f) * ss * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0)   r = 3.14159274f - r;
    return y < 0 ? -r : r;
}
static int32_t    *s_span0, *s_span1;   /* x range the sphere occupies, per row */
static float       s_lon;        /* current longitude (0..1) */
static float       s_spin;       /* turns per second */
static float       s_tilt;       /* vertical tilt of the axis (radians) */
static float       s_tilt_v;     /* and its momentum */
/* 🚨 How far the axis leans sideways. Without this it is a turntable with its
 * axis nailed to the screen's vertical. A sphere gets away with that because
 * the markings drift diagonally and look plausible, but a ringed planet is
 * perfectly symmetric about its axis and simply did not respond to being
 * spun in place — the rings only opened and closed, long axis stuck
 * horizontal forever. */
static float       s_lean;        /* sideways lean of the axis (radians) */
static float       s_lean_v;      /* and its momentum */
static bool        s_coarse;      /* moving — draw it coarse */
/* 🚨 Whether this table is actually a win has **not been measured on the
 * board**. On the PC simulator it was slower: x86 has cheap trigonometry and
 * a large cache, so reading a table costs more than computing the value. The
 * board may well be the opposite (expensive trigonometry, 32 KB of cache).
 *
 * But the real wall may not be arithmetic at all — it may be **reading the
 * texture**. A 256 KB image read with per-pixel jumps misses cache
 * constantly. If so, this table saves nothing and spends 532 KB of PSRAM.
 * → Measure, and if it does not help, set the flag below to 0; that one line
 *   removes the whole thing.
 *   Worth measuring alongside: shrinking the texture from 512x256 to 256x128,
 *   which fits cache far better. That may be the real answer. */
/*
 * Solving the inverse trigonometry per pixel is the most expensive part of
 * this render — but the answer does not change while it is only spinning
 * horizontally, because longitude is added as an integer. So while the
 * attitude (tilt and lean) holds still, the texture coordinates are cached
 * per pixel and later frames only read the table. A change in attitude
 * refills it that frame. */
#define ORB_UV_TABLE 1           /* set to 0 and the table is not used at all */
static uint16_t   *s_lut_tu;     /* per-pixel texture column (before longitude) */
static uint16_t   *s_lut_tv;     /* per-pixel texture row */
static bool        s_uv_ok;      /* does the table match the current attitude? */
static float       s_uv_tilt, s_uv_lean;
static uint32_t    s_last_ms;
static uint32_t    s_render_us;
static bool        s_dirty;
static int32_t     s_drag_x, s_drag_y;
static bool        s_dragging;
static orb_kind_t  s_kind;

/* ── noise ────────────────────────────────────────────────────
 * Value noise at several scales. It has to wrap along longitude or the seam shows. */
static uint32_t hash2(int x, int y)
{
    /* 🚨 Multiply as unsigned. Done in int these overflow, which is undefined
     * behaviour, and GCC says so the moment it can fold a constant argument —
     * bake_sun's hash2(i * 37, 991) made it shout. The bits are identical. */
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float vnoise(float x, float y, int period)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    #define N(a,b) ((float)(hash2(((a) % period + period) % period, b) & 0xFFFF) / 65535.0f)
    float n00 = N(xi, yi),     n10 = N(xi + 1, yi);
    float n01 = N(xi, yi + 1), n11 = N(xi + 1, yi + 1);
    #undef N
    return (n00 * (1 - u) + n10 * u) * (1 - v) + (n01 * (1 - u) + n11 * u) * v;
}
static float fbm(float x, float y, int oct, int period)
{
    float sum = 0, amp = 0.5f, tot = 0;
    int p = period;
    for (int i = 0; i < oct; i++) {
        sum += vnoise(x, y, p) * amp;
        tot += amp;
        x *= 2; y *= 2; p *= 2; amp *= 0.5f;
    }
    return sum / tot;
}

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ── generating the texture ───────────────────────────────────
 * 🚨 This is made in code. The Moon is greyscale with craters and comes out
 * quite convincing; the Earth's continents are simply not Earth's — it is an
 * Earth-like planet, not Earth. For the real thing use
 * tools/make-orb-texture.py. */
static void bake_moon(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            /* Base — a quiet grey */
            float base = 0.62f + 0.16f * fbm(fx * 8, fy * 8, 5, 8);
            /* Maria (dark basalt plains) — a few large patches */
            float mare = fbm(fx * 3 + 11.3f, fy * 3 + 4.7f, 3, 3);
            if (mare < 0.42f) base *= 0.62f + 0.5f * (mare / 0.42f);
            /* Craters — one per grid cell, with a bright rim */
            float c = 0;
            for (int lvl = 0; lvl < 3; lvl++) {
                int per = 12 << lvl;
                float gx = fx * per, gy = fy * per;
                int cx = (int)floorf(gx), cy = (int)floorf(gy);
                for (int oy = -1; oy <= 1; oy++)
                    for (int ox = -1; ox <= 1; ox++) {
                        uint32_t h = hash2(((cx + ox) % per + per) % per, cy + oy);
                        float px = cx + ox + (float)(h & 0xFF) / 255.0f;
                        float py = cy + oy + (float)((h >> 8) & 0xFF) / 255.0f;
                        float rad = 0.18f + 0.26f * (float)((h >> 16) & 0xFF) / 255.0f;
                        if (((h >> 24) & 0xFF) < 150) continue;      /* not every cell gets one */
                        float d = sqrtf((gx - px) * (gx - px) + (gy - py) * (gy - py));
                        if (d > rad) continue;
                        float t = d / rad;
                        /* Dark inside, bright around the edge */
                        c += (t > 0.78f) ? (0.20f * (1 - fabsf(t - 0.89f) / 0.11f))
                                         : (-0.16f * (1 - t));
                    }
            }
            int v = (int)((base + c) * 255.0f);
            s_tex_owned[y * TEX_W + x] = rgb565(v, v, (int)(v * 0.97f));
        }
    }
}

static void bake_earth(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        float lat = (0.5f - fy) * 2.0f;              /* +1 north pole .. -1 south */
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            float h = fbm(fx * 6, fy * 6, 6, 6);
            /* Squeeze land at higher latitudes so it clumps into continents */
            h -= 0.10f * fabsf(lat);
            int r, g, b;
            if (h < 0.46f) {                          /* ocean, shaded by depth */
                float d = h / 0.46f;
                r = (int)(6 + 18 * d); g = (int)(28 + 60 * d); b = (int)(78 + 90 * d);
            } else {                                  /* land */
                float t = (h - 0.46f) / 0.54f;
                r = (int)(48 + 70 * t); g = (int)(96 + 60 * t); b = (int)(44 + 40 * t);
                /* Desert. A hard cutoff looks blocky, so it is blended. */
                float dry = fbm(fx * 11 + 5.5f, fy * 11 + 2.5f, 4, 11);
                float band = 1.0f - fabsf(lat) / 0.40f;          /* strongest near the equator */
                float k = (dry - 0.50f) / 0.22f * (band > 0 ? band : 0);
                if (k > 0) {
                    if (k > 1) k = 1;
                    r = (int)(r * (1 - k) + 178 * k);
                    g = (int)(g * (1 - k) + 150 * k);
                    b = (int)(b * (1 - k) +  98 * k);
                }
                if (t > 0.62f) {                                  /* mountains */
                    float m = (t - 0.62f) / 0.38f;
                    int sv = (int)(150 + 90 * m);
                    r = (int)(r * (1 - m) + sv * m);
                    g = (int)(g * (1 - m) + sv * m);
                    b = (int)(b * (1 - m) + sv * m);
                }
            }
            /* Ice caps */
            float ice = (fabsf(lat) - 0.78f) / 0.22f;
            if (ice > 0) {
                float k = ice > 1 ? 1 : ice;
                r = (int)(r * (1 - k) + 244 * k);
                g = (int)(g * (1 - k) + 248 * k);
                b = (int)(b * (1 - k) + 255 * k);
            }
            /* One layer of cloud */
            float cl = fbm(fx * 9 + 21.1f, fy * 9 + 13.7f, 5, 9);
            if (cl > 0.58f) {
                float k = (cl - 0.58f) / 0.42f; if (k > 0.8f) k = 0.8f;
                r = (int)(r * (1 - k) + 250 * k);
                g = (int)(g * (1 - k) + 250 * k);
                b = (int)(b * (1 - k) + 252 * k);
            }
            s_tex_owned[y * TEX_W + x] = rgb565(r, g, b);
        }
    }
}

/* Jupiter. The belts and zones are bands of latitude, which on their own look
 * like a deck chair. What makes them read as Jupiter is that the boundary
 * between two bands is pushed sideways by turbulence stretched along
 * longitude — the flow is zonal, so the noise has to be too, and the edges
 * curl instead of running straight. */
static void bake_jupiter(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        float lat = (0.5f - fy) * 2.0f;              /* +1 north pole .. -1 south */
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            /* Six times the detail across latitude as along longitude */
            float warp = fbm(fx * 4, fy * 24, 5, 4) - 0.5f;
            float l    = lat + warp * 0.13f;

            /* Alternating bands. The cosine sets how many; raising it to a
             * power pinches the light zones so the dark belts dominate. */
            float band = 0.5f + 0.5f * cosf(l * 14.5f);
            float t    = band * band * (3 - 2 * band);          /* 0 belt .. 1 zone */

            /* belt: red-brown · zone: pale cream */
            int r = (int)(168 + (232 - 168) * t);
            int g = (int)(118 + (216 - 118) * t);
            int b = (int)( 78 + (186 -  78) * t);

            /* Fine streaks along the flow, so a band is not flat colour */
            float streak = fbm(fx * 8 + 3.1f, fy * 48 + 1.7f, 4, 8) - 0.5f;
            r += (int)(streak * 34); g += (int)(streak * 30); b += (int)(streak * 24);

            /* The poles are cooler and hazier than the tropics */
            float pole = (fabsf(lat) - 0.62f) / 0.38f;
            if (pole > 0) {
                float k = pole > 1 ? 1 : pole;
                k *= 0.72f;
                r = (int)(r * (1 - k) + 132 * k);
                g = (int)(g * (1 - k) + 126 * k);
                b = (int)(b * (1 - k) + 134 * k);
            }

            /* The Great Red Spot. An ellipse wider than it is tall, with a
             * paler collar — a hard edge looks like a sticker. */
            float dx = fx - 0.63f;
            if (dx >  0.5f) dx -= 1.0f;                 /* it must wrap at the seam */
            if (dx < -0.5f) dx += 1.0f;
            /* 🚨 The radii are not in the same units. The map is 512 wide and
             * 256 tall while latitude spans 2, so a latitude radius has to be
             * four times a longitude one to draw the same shape — written as
             * 0.105 and 0.052 the spot came out eight times wider than tall,
             * a red stripe across the planet. */
            float dy = (lat - (-0.23f)) * 0.25f;
            float d  = sqrtf((dx / 0.105f) * (dx / 0.105f) + (dy / 0.048f) * (dy / 0.048f));
            if (d < 1.4f) {
                /* One falloff for the whole thing. Two — a body and a collar —
                 * left a visible step where they met. */
                float k = 1.0f - d / 1.4f;
                k = k * k * (3 - 2 * k);
                r = (int)(r * (1 - k) + 186 * k);
                g = (int)(g * (1 - k) +  96 * k);
                b = (int)(b * (1 - k) +  72 * k);
            }
            s_tex_owned[y * TEX_W + x] = rgb565(r, g, b);
        }
    }
}

/* The Sun. Granulation is the whole picture: convection cells a shade brighter
 * in the middle with darker lanes between them. Two scales of noise give the
 * cells and the grain inside them; a handful of spots break up the surface so
 * that turning it actually reads as turning. */
static void bake_sun(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        float lat = (0.5f - fy) * 2.0f;
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            float cell = fbm(fx * 22, fy * 22, 3, 22);
            float fine = fbm(fx * 52 + 7.3f, fy * 52 + 2.1f, 3, 52);
            float v = 0.86f + 0.34f * (cell - 0.5f) + 0.20f * (fine - 0.5f);

            /* Faculae — the bright network */
            float fac = fbm(fx * 10 + 17.9f, fy * 10 + 4.3f, 4, 10);
            if (fac > 0.60f) v += (fac - 0.60f) * 0.55f;

            /* Spots, in two belts either side of the equator as they really
             * are. 🚨 The darkening multiplies the finished colour rather than
             * v — folded into v it only pulls the brightness down and the spot
             * reads as a red hole instead of a dark one. */
            float dark = 1.0f;
            for (int i = 0; i < 7; i++) {
                uint32_t h = hash2(i * 37, 991);
                float sx  = (float)(h & 0xFFFF) / 65535.0f;
                float sl  = ((i & 1) ? 1.0f : -1.0f) *
                            (0.14f + 0.26f * (float)((h >> 16) & 0xFF) / 255.0f);
                float rad = 0.019f + 0.022f * (float)((h >> 24) & 0xFF) / 255.0f;
                float ddx = fx - sx;
                if (ddx >  0.5f) ddx -= 1.0f;
                if (ddx < -0.5f) ddx += 1.0f;
                /* 🚨 The map is 512 wide and 256 tall while latitude spans 2,
                 * so a degree of latitude covers four times the texture that a
                 * degree of longitude does. Without this factor the spots come
                 * out as flat ovals. */
                float ddy = (lat - sl) * 0.25f;
                float d = sqrtf(ddx * ddx + ddy * ddy);
                if (d > rad * 2.1f) continue;
                if (d < rad) {                           /* umbra */
                    dark *= 0.16f + 0.16f * (d / rad);
                } else {                                 /* penumbra, faded out */
                    float t = (d - rad) / (rad * 1.1f);
                    t = t * t * (3 - 2 * t);             /* soft, or the rim is a hard ring */
                    dark *= 0.32f + 0.68f * t;
                }
            }

            /* Hot where it is bright, dropping to a deep orange in the lanes
             * — blue falls away fastest, which is what makes it read as heat
             * rather than as a yellow ball. */
            int r = (int)(252 * (0.55f + 0.45f * v) * dark);
            int g = (int)(196 * v * v * dark);
            int b = (int)( 96 * v * v * v * dark);
            s_tex_owned[y * TEX_W + x] = rgb565(r, g, b);
        }
    }
}

/* ── lookup tables ────────────────────────────────────────────
 * For a screen point (dx,dy), which latitude and longitude is it? The
 * attitude does not change, so this is computed once. Rotation is an addition
 * to the longitude. */
static bool build_lut(void)
{
    size_t px = (size_t)(2 * ORB_R + 1) * (2 * ORB_R + 1);
    s_lut_shade = heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
    s_lut_z     = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    s_span0     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_span1     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_col_x     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_asin_tab  = heap_caps_malloc(1025 * 4, MALLOC_CAP_SPIRAM);
#if ORB_UV_TABLE
    s_lut_tu    = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    s_lut_tv    = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
#else
    s_lut_tu = s_lut_tv = NULL;
#endif
    s_uv_ok = false;
    if (!s_lut_shade || !s_lut_z || !s_span0 || !s_span1 || !s_col_x || !s_asin_tab)
        return false;
    /* The texture-coordinate table is optional — without it this is only slower. */
    if (!s_lut_tu || !s_lut_tv) {
        if (s_lut_tu) { heap_caps_free(s_lut_tu); s_lut_tu = NULL; }
        if (s_lut_tv) { heap_caps_free(s_lut_tv); s_lut_tv = NULL; }
    }

    for (int i = 0; i <= 2 * ORB_R; i++) s_col_x[i] = (float)(i - ORB_R) / ORB_R;
    for (int i = 0; i <= 1024; i++) {
        float t = (float)i / 512.0f - 1.0f;
        if (t >  1) t =  1;
        if (t < -1) t = -1;
        s_asin_tab[i] = asinf(t);
    }

    /* Light from the upper left, in front. Straight-on light looks flat.
     * 🚨 Brightness follows the normal as seen on screen, and which part of
     * the ball a pixel is does not change however the globe is turned — so it
     * only has to be worked out once. */
    /* 🚨 yy is taken as "+ is up the screen". It used to use pixel
     * coordinates directly (+ is down), which flipped latitude entirely: the
     * top of the screen was the south pole. Both poles are white so the eye
     * does not catch it, and upside-down continents are unrecognisable rather
     * than obviously wrong. The light uses the same convention. */
    const float lx = -0.45f, ly = 0.42f, lz = 0.79f;

    for (int j2 = 0; j2 <= 2 * ORB_R; j2++) {
        int dy = j2 - ORB_R;
        float yy = -(float)dy / ORB_R;      /* up the screen is north */
        float rowr2 = 1.0f - yy * yy;
        if (rowr2 <= 0) { s_span0[j2] = 1; s_span1[j2] = 0; continue; }
        int half = (int)(ORB_R * sqrtf(rowr2));
        s_span0[j2] = ORB_R - half;
        s_span1[j2] = ORB_R + half;

        for (int i2 = s_span0[j2]; i2 <= s_span1[j2]; i2++) {
            int dx = i2 - ORB_R;
            float xx = (float)dx / ORB_R;
            float zz2 = 1.0f - xx * xx - yy * yy;
            if (zz2 < 0) zz2 = 0;
            float zz = sqrtf(zz2);

            /* 🚨 The Sun emits its own light and must not have a shadow.
             * Only the rim is darkened slightly so it still reads as a ball. */
            if (s_kind == ORB_SUN) {
                s_lut_shade[(size_t)j2 * (2 * ORB_R + 1) + i2] =
                    (uint8_t)((0.88f + 0.12f * zz) * 255.0f);
                s_lut_z[(size_t)j2 * (2 * ORB_R + 1) + i2] = (int16_t)(zz * 32767.0f);
                continue;
            }
            float nl = xx * lx + yy * ly + zz * lz;   /* Lambert */
            if (nl < 0) nl = 0;
            /* Satellite imagery already has sunlight in it, so multiplying
             * hard darkens it twice.
             * 🚨 Earth alone needs lifting. Measured texture brightness
             * (median, out of 255): Earth 41.3, Moon 140.4, Jupiter 159.0,
             * Sun 148.7. Ocean covers most of the frame and is very dark,
             * while only the clouds at 243 pull the mean up. Raising the
             * lighting for everything would blow out the Moon and Jupiter, so
             * only Earth gets a lifted floor. The texture itself is
             * brightened with gamma — a curve that raises the dark end a lot
             * and the bright end barely, so the clouds do not clip. */
            /* 🚨 Gamma 0.38 with a 0.55 floor came back as "too bright", so
             * both came down a step: effective gamma 0.50, floor 0.50. */
            float amb = (s_kind == ORB_EARTH) ? 0.50f : 0.42f;
            float rim = (s_kind == ORB_EARTH) ? 0.78f : 0.74f;
            float sh = amb + (1.0f - amb) * nl;
            sh *= rim + (1.0f - rim) * zz;            /* only at the rim */
            if (sh > 1.0f) sh = 1.0f;
            size_t idx = (size_t)j2 * (2 * ORB_R + 1) + i2;
            s_lut_shade[idx] = (uint8_t)(sh * 255.0f);
            s_lut_z[idx] = (int16_t)(zz * 32767.0f);
        }
    }
    return true;
}

/* ── one frame ────────────────────────────────────────────────
 * All it does is read the table, fetch a texel, and multiply by brightness. */
static void render(void)
{
    int64_t t0 = esp_timer_get_time();
    float ct = cosf(s_tilt), st = sinf(s_tilt);
    /* 🚨 A leaning axis is the same thing as rotating the whole screen, so
     * un-rotating the screen coordinates at the end leaves every other
     * calculation — and the table — untouched.
     * Brightness and depth are not un-rotated: light is fixed to the world
     * and does not turn with the planet, and depth depends only on distance
     * from the centre. */
    float cl = cosf(s_lean), sl = sinf(s_lean);
    bool  leaning = (s_lean != 0.0f);
    int   rot = (int)(s_lon * TEX_W);
    int   stride = 2 * ORB_R + 1;

    const float U2TEX = TEX_W / 6.2831853f;
    const float V2TEX = TEX_H / 3.1415927f;

    /* 🚨 Solving inverse trigonometry per pixel made a frame take 135 ms
     * (7 fps). While it is being spun the eye cannot follow the detail, so it
     * draws in 2x2 blocks and does a quarter of the work; when it stops, one
     * full-resolution frame is drawn. The still frame is the one you end up
     * looking at, and that is the one that has to be sharp. */
    /* If the attitude has not changed, draw from the table — the per-pixel
     * inverse trigonometry drops out. There is no reason to go coarse then,
     * so it runs at full resolution. */
    bool use_uv = (s_lut_tu && s_uv_ok && s_uv_tilt == s_tilt && s_uv_lean == s_lean);
    bool fill_uv = (s_lut_tu && !use_uv && !s_coarse);
    int step = (s_coarse && !use_uv) ? 2 : 1;

    for (int j = 0; j <= 2 * ORB_R; j += step) {
        int y = CY - ORB_R + j;
        if (y < 0 || y >= 466) continue;
        int i0 = s_span0[j], i1 = s_span1[j];
        if (i0 > i1) continue;
        uint16_t *out = s_fb + (size_t)y * 466;
        const uint8_t *sd = s_lut_shade + (size_t)j * stride;
        const int16_t *zl = s_lut_z     + (size_t)j * stride;

        /* y only has to be computed once per row. Up the screen is north. */
        float yy = -(float)(j - ORB_R) / ORB_R;
        float yc = yy * ct, ys = yy * st;

        /* When it leans, walk the un-rotated coordinates along the row */
        float oy = (float)(j - ORB_R), ox0 = (float)(i0 - ORB_R);
        float inv_R = 1.0f / ORB_R;
        float xr = (ox0 * cl + oy * sl) * inv_R, xr_d =  cl * inv_R;
        float yr = -(-ox0 * sl + oy * cl) * inv_R, yr_d = sl * inv_R;

        uint16_t *ru = s_lut_tu ? s_lut_tu + (size_t)j * stride : NULL;
        uint16_t *rv = s_lut_tv ? s_lut_tv + (size_t)j * stride : NULL;

        /* ── fast path: straight from the table ─────────────── */
        if (use_uv) {
            for (int i = i0; i <= i1; i++) {
                int x = CX - ORB_R + i;
                if (x < 0 || x >= 466) continue;
                int tu = (ru[i] + rot) & (TEX_W - 1);
                uint16_t c = s_tex[(size_t)rv[i] * TEX_W + tu];
                uint32_t sh = sd[i];
                uint32_t r = ((c >> 11) & 0x1F) * sh >> 8;
                uint32_t g = ((c >> 5)  & 0x3F) * sh >> 8;
                uint32_t b = ( c        & 0x1F) * sh >> 8;
                out[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
            continue;
        }

        uint16_t first_px = 0, last_px = 0;   /* for filling the edges in coarse mode */
        for (int i = i0; i <= i1; i += step, xr += xr_d * step, yr += yr_d * step) {
            int x = CX - ORB_R + i;
            if (x < 0 || x >= 466) continue;

            float cx = s_col_x[i];
            if (leaning) { cx = xr; yc = yr * ct; ys = yr * st; }

            float zz = zl[i] * (1.0f / 32767.0f);
            /* Tilt about the X axis. Horizontal rotation is an integer add below. */
            float y2 = yc - zz * st;
            float z2 = ys + zz * ct;

            /* Longitude — polynomial approximation. The table holds it before longitude is added. */
            int tu0 = (int)(atan2_fast(cx, z2) * U2TEX) & (TEX_W - 1);
            int tu = (tu0 + rot) & (TEX_W - 1);

            /* Latitude — table lookup with interpolation */
            float fa = (y2 + 1.0f) * 512.0f;
            int   ia = (int)fa;
            if (ia < 0) ia = 0;
            if (ia > 1023) ia = 1023;
            float lat = s_asin_tab[ia] + (s_asin_tab[ia + 1] - s_asin_tab[ia]) * (fa - ia);
            int tv = (int)(TEX_H * 0.5f - lat * V2TEX);
            if (tv < 0) tv = 0;
            if (tv >= TEX_H) tv = TEX_H - 1;

            if (fill_uv) { ru[i] = (uint16_t)tu0; rv[i] = (uint16_t)tv; }

            uint16_t c = s_tex[(size_t)tv * TEX_W + tu];
            uint32_t sh = sd[i];
            uint32_t r = ((c >> 11) & 0x1F) * sh >> 8;
            uint32_t g = ((c >> 5)  & 0x3F) * sh >> 8;
            uint32_t b = ( c        & 0x1F) * sh >> 8;
            uint16_t px = (uint16_t)((r << 11) | (g << 5) | b);
            out[x] = px;
            if (i == i0) first_px = px;
            last_px = px;
            if (step == 2) {                 /* spread the same colour right and down */
                if (x + 1 < 466 && i + 1 <= i1) out[x + 1] = px;
                if (y + 1 < 466) {
                    uint16_t *o2 = s_fb + (size_t)(y + 1) * 466;
                    int j2 = j + 1;
                    if (j2 <= 2 * ORB_R && i >= s_span0[j2] && i <= s_span1[j2]) {
                        o2[x] = px;
                        if (x + 1 < 466 && i + 1 <= s_span1[j2]) o2[x + 1] = px;
                    }
                }
            }
        }
        /* 🚨 Where the row below is wider than the one above (near the poles)
         * there is no colour to copy from, so the previous frame's pixels
         * remain — the edge frays while it turns. Fill that much with the
         * end colours. */
        if (step == 2 && j + 1 <= 2 * ORB_R) {
            int j2 = j + 1, y2 = y + 1;
            if (y2 < 466 && s_span0[j2] <= s_span1[j2]) {
                uint16_t *o2 = s_fb + (size_t)y2 * 466;
                for (int i = s_span0[j2]; i < i0; i++) {
                    int x2 = CX - ORB_R + i;
                    if (x2 >= 0 && x2 < 466) o2[x2] = first_px;
                }
                for (int i = i1 + 1; i <= s_span1[j2]; i++) {
                    int x2 = CX - ORB_R + i;
                    if (x2 >= 0 && x2 < 466) o2[x2] = last_px;
                }
            }
        }
    }
    if (fill_uv) { s_uv_ok = true; s_uv_tilt = s_tilt; s_uv_lean = s_lean; }
    s_render_us = (uint32_t)(esp_timer_get_time() - t0);
}


/* ── frame-time meter ─────────────────────────────────────────
 * 🔋 Measuring power starts with knowing how much CPU a frame costs.
 * Averaged over 128 and logged as one line — the logging itself must not
 * become the cost. */
static void frame_tick(const char *who, int64_t t0)
{
    static uint32_t n; static uint64_t sum; static uint32_t hi; static int64_t since;
    /* 🚨 Timing only the computation is half the story: it leaves out what
     * LVGL then paints and what is pushed to the panel. The water app was the
     * clearest case — build_spans() only prepares data and the real painting
     * happens later, so it reported "25 fps" while the screen plainly was
     * not. Measure the **real interval** until this function is called again.
     * That is the speed a person sees. */
    static int64_t prev; static uint64_t gap; static uint32_t gapn; static uint32_t gaphi;
    int64_t now = esp_timer_get_time();
    uint32_t us = (uint32_t)(now - t0);
    sum += us; if (us > hi) hi = us;
    if (prev && now - prev < 2000000) {          /* skip it if the app was just opened */
        uint32_t g = (uint32_t)(now - prev);
        gap += g; gapn++; if (g > gaphi) gaphi = g;
    }
    prev = now;
    if (!since) since = now;
    /* 🚨 This used to log every 128 frames, which meant something slow like
     * the water never reached the count and never printed at all. The slower
     * it got, the more silent it became — exactly backwards. Every three
     * seconds now. */
    if (++n && now - since >= 3000000) {
        since = now;
        ESP_LOGI(who, "compute %u us (max %u) - real interval %u us -> %u fps  [%u frames]",
                 (unsigned)(sum / n), (unsigned)hi,
                 (unsigned)(gapn ? gap / gapn : 0),
                 (unsigned)(gap && gapn ? 1000000ULL * gapn / gap : 0),
                 (unsigned)n);
        n = 0; sum = 0; hi = 0; gap = 0; gapn = 0; gaphi = 0;
    }
}

static void step(lv_timer_t *t)
{
    /* 🔋 Nothing to draw with the display off. 🚨 Do not change the period
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so from a timer callback the handler restarts and never returns.
     * Leave the period and act on every 60th call. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 60) return;
    }

    uint32_t now = lv_tick_get();
    float dt = s_last_ms ? (now - s_last_ms) / 1000.0f : 0.033f;
    s_last_ms = now;
    if (dt > 0.2f) dt = 0.2f;

    if (!s_dragging) {
        s_spin *= powf(0.35f, dt);            /* friction */
        if (fabsf(s_spin) < 0.004f) s_spin = 0;
        s_lon += s_spin * dt;
        s_tilt_v *= powf(0.30f, dt);
        if (fabsf(s_tilt_v) < 0.01f) s_tilt_v = 0;
        s_tilt += s_tilt_v * dt;
        s_lean_v *= powf(0.30f, dt);
        if (fabsf(s_lean_v) < 0.01f) s_lean_v = 0;
        s_lean += s_lean_v * dt;
    }
    /* Not clamped. Keep flicking and it goes over the pole and looks at the
     * far side upside down. (Back when this interpolated on a grid the poles
     * tore and it had to be clamped; solving per pixel, looking straight at a
     * pole is clean.) */
    while (s_tilt >  3.1415927f) s_tilt -= 6.2831853f;
    while (s_tilt < -3.1415927f) s_tilt += 6.2831853f;
    while (s_lean >  3.1415927f) s_lean -= 6.2831853f;
    while (s_lean < -3.1415927f) s_lean += 6.2831853f;
    if (s_lon < 0) s_lon += 1.0f;
    if (s_lon >= 1.0f) s_lon -= 1.0f;

    /* 🚨 Not turning means not a single pixel is drawn. Redrawing the whole
     * screen is the most expensive thing this device does, so at rest this
     * costs the same as any static screen. The timer slows down too, so the
     * CPU wakes less. */
    bool moving = (s_spin != 0 || s_tilt_v != 0 || s_lean_v != 0 || s_dragging);
    /* 🚨 If it has just stopped, one sharp frame must be drawn. This check has
     * to come before the "skip it if it is not turning" below — after it, the
     * moment it stops it skips, and the last coarse frame is what stays. */
    /* 🚨 A change in attitude invalidates the table. The next full frame refills it. */
    if (s_uv_ok && (s_uv_tilt != s_tilt || s_uv_lean != s_lean)) s_uv_ok = false;
    /* With a valid table, full resolution is cheap — no reason to go coarse */
    bool can_fast = (s_lut_tu && s_uv_ok);
    if (!moving && s_coarse) { s_dirty = true; }
    s_coarse = moving && !can_fast;

    if (!moving && !s_dirty) {
        /* Nothing to draw when it is still. Keep the period and look every 6th call. */
        static uint8_t idle;
        if (++idle % 6) return;
        return;
    }
    s_dirty = false;
    int64_t _t0 = esp_timer_get_time();
    render();
    lv_obj_invalidate(s_canvas);
    frame_tick("orb", _t0);
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);

    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        s_drag_x = p.x;
        s_drag_y = p.y;
        s_spin = 0;
        s_tilt_v = 0;
        s_lean_v = 0;
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dx = p.x - s_drag_x, dy = p.y - s_drag_y;
        if (dx || dy) {
            s_drag_x = p.x;
            s_drag_y = p.y;
            /* Dragging one screen diameter horizontally is half a turn.
             * 🚨 Past the pole it is flipped, so you are looking at the far
             * side and longitude reads mirrored — left alone it turns opposite
             * to the finger. While flipped, the sign is inverted so it
             * follows the hand. */
            /* 🚨 Where you grab the ball matters. Drag across the middle and
             * it spins in place; grab the top and push sideways and the whole
             * ball tips — which is what a real ball does. The grab point
             * decides between the two. Without this the axis is nailed to the
             * screen's vertical. */
            float gx = (float)(p.x - CX) / ORB_R;
            float gy = (float)(p.y - CY) / ORB_R;
            if (gx >  1) gx =  1;
            if (gx < -1) gx = -1;
            if (gy >  1) gy =  1;
            if (gy < -1) gy = -1;

            /* Dragging near the edge rolls the ball where it sits and leans
             * the axis. This is measured in screen terms, before un-rotating. */
            float dln = ((float)dx * -gy + (float)dy * gx)
                        / (2.0f * ORB_R) * 3.1415927f;

            /* 🚨 With the axis leaning, the finger's movement has to be
             * un-rotated by the same amount. Spin and tilt happen about the
             * ball's axis, but the screen shows it leaned over. Without this,
             * at about 90 degrees a horizontal drag becomes vertical and at
             * 180 it runs backwards — which is what "the moon's direction is
             * inverted again" was. The grab point is un-rotated too. */
            float cl = cosf(s_lean), sl = sinf(s_lean);
            float bx =  (float)dx * cl + (float)dy * sl;
            float by = -(float)dx * sl + (float)dy * cl;
            float bgx =  gx * cl + gy * sl;
            float bgy = -gx * sl + gy * cl;

            float flip = (cosf(s_tilt) >= 0) ? 1.0f : -1.0f;
            float dl = -bx / (2.0f * ORB_R) * 0.5f * flip
                       * (1.0f - fabsf(bgy));
            s_lon += dl;
            s_spin = dl * 12.0f;                       /* momentum on release */

            s_lean += dln;
            s_lean_v = dln * 12.0f;
            /* Vertically, one diameter of drag is 180 degrees. A diagonal
             * drag feeds both.
             * 🚨 Fixing north and south (up the screen is north) inverted the
             * vertical direction too. The sign is matched so it turns the way
             * you drag. */
            float dt2 = -by / (2.0f * ORB_R) * 3.1415927f
                        * (1.0f - fabsf(bgx));
            s_tilt += dt2;
            s_tilt_v = dt2 * 12.0f;
            s_dirty = true;
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_dragging = false;
    }
}

lv_timer_t *orb_start(lv_obj_t *root, orb_kind_t kind)
{
    int64_t _open0 = esp_timer_get_time();
    s_kind = kind;
    s_lon = 0; s_spin = 0.10f;       /* it turns slowly on arrival, to say hello */
    s_tilt = 0; s_tilt_v = 0;
    s_lean = 0; s_lean_v = 0;
    s_last_ms = 0; s_dirty = true; s_dragging = false;

    s_fb = heap_caps_malloc((size_t)466 * 466 * 2, MALLOC_CAP_SPIRAM);

    s_tex = NULL;
#ifdef HAVE_MOON_TEX
    if (kind == ORB_MOON)    s_tex = ORB_TEX_MOON;
#endif
#ifdef HAVE_EARTH_TEX
    if (kind == ORB_EARTH)   s_tex = ORB_TEX_EARTH;
#endif
#ifdef HAVE_SUN_TEX
    if (kind == ORB_SUN)     s_tex = ORB_TEX_SUN;
#endif
#ifdef HAVE_JUPITER_TEX
    if (kind == ORB_JUPITER) s_tex = ORB_TEX_JUPITER;
#endif
    s_tex_bake = false;
    if (!s_tex) {   /* nothing baked in — generate one */
        s_tex_owned = heap_caps_malloc((size_t)TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
        s_tex = s_tex_owned;
        s_tex_bake = true;
    } else {
        /* 🚨 A baked texture lives in flash, and this reads it with per-pixel
         * jumps. Flash is four-wire (QSPI), so every cache miss waits a long
         * time; PSRAM is eight-wire and the same miss costs half. The 256 KB
         * is copied there and read from there. */
        uint16_t *fast = heap_caps_malloc((size_t)TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
        if (fast) {
            memcpy(fast, s_tex, (size_t)TEX_W * TEX_H * 2);
            s_tex_owned = fast;
            s_tex = fast;
        }
    }
    if (!s_fb || !s_tex || !build_lut()) {
        ESP_LOGE("orb", "out of memory — skipping the globe");
        orb_stop();
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, "no memory");
        lv_obj_center(l);
        return NULL;
    }
    memset(s_fb, 0, (size_t)466 * 466 * 2);
    /* 🚨 Every kind needs its own bake. This used to fall through to
     * bake_earth() for anything that was not the Moon, so with no photograph
     * baked in the Sun and Jupiter both came out as the Earth. */
    if (s_tex_bake) {
        switch (kind) {
        case ORB_MOON:    bake_moon();    break;
        case ORB_SUN:     bake_sun();     break;
        case ORB_JUPITER: bake_jupiter(); break;
        default:          bake_earth();   break;
        }
    }

    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_fb, 466, 466, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_RELEASED, NULL);

    render();
    s_loop = lv_timer_create(step, 33, NULL);
    ESP_LOGI("orb", "%s opened in %u ms (texture: %s)", ORB_NAME[kind],
             (unsigned)((esp_timer_get_time() - _open0) / 1000),
             s_tex_bake ? "generated" : (s_tex_owned ? "copied to PSRAM" : "read from flash"));
    return s_loop;
}

void orb_stop(void)
{
    s_loop = NULL;                 /* the caller deletes the timer */
    /* 🚨 Delete the image before freeing the buffer. The other way round
     * leaves the canvas pointing at memory that is already gone, and it is
     * read during the delete (this killed the simulator on the third open). */
    if (s_canvas) { lv_obj_delete(s_canvas); s_canvas = NULL; }
    /* Always give the PSRAM back. 434 KB plus 260 KB plus tables fills up fast. */
    if (s_fb)        { heap_caps_free(s_fb);        s_fb = NULL; }
    s_tex = NULL;
    if (s_tex_owned) { heap_caps_free(s_tex_owned); s_tex_owned = NULL; }
    if (s_lut_shade) { heap_caps_free(s_lut_shade); s_lut_shade = NULL; }
    if (s_lut_z)     { heap_caps_free(s_lut_z);     s_lut_z = NULL; }
    if (s_col_x)     { heap_caps_free(s_col_x);     s_col_x = NULL; }
    if (s_asin_tab)  { heap_caps_free(s_asin_tab);  s_asin_tab = NULL; }
    if (s_lut_tu)    { heap_caps_free(s_lut_tu);    s_lut_tu = NULL; }
    if (s_lut_tv)    { heap_caps_free(s_lut_tv);    s_lut_tv = NULL; }
    s_uv_ok = false;
    if (s_span0)     { heap_caps_free(s_span0);     s_span0 = NULL; }
    if (s_span1)     { heap_caps_free(s_span1);     s_span1 = NULL; }
}

/* For tests — set the tilt directly in degrees. Getting the worst-case angle
 * (looking straight down a pole) by hand is hard because of the momentum. */
void orb_set_lean_deg(float deg) /* for tests — lean the axis sideways */
{
    s_lean = deg * 0.0174533f;
    s_lean_v = 0;
    s_dirty = true;
}

void orb_set_lon(float lon)      /* for tests — which longitude faces front (0..1) */
{
    s_lon = lon;
    s_dirty = true;
}

void orb_set_tilt_deg(float deg)
{
    s_tilt = deg * 0.0174533f;
    s_tilt_v = 0;
    s_dirty = true;
}

void orb_debug(float *lon, uint32_t *render_us)
{
    if (lon) *lon = s_lon;
    if (render_us) *render_us = s_render_us;
}
