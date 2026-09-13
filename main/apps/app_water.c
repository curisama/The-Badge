/* Water — an actual particle fluid, with a boat on it.
 *
 * 🚨 Three attempts were thrown away before this one.
 *   1) fixed sine waves on a tilted plane -> a sheet of cardboard
 *   2) two standing waves mixed -> however you shook it, the same shape
 *   3) shallow-water equations (one height per column) -> the physics was
 *      right and it was still a *waveform*. With one y per x there is
 *      structurally nowhere to put a droplet, a breaking wave, or a blob
 *      that has come away. As long as it is a height field, no amount of
 *      fixing the equations makes it water.
 *
 * So the water is made of particles. Several hundred droplets push each other
 * around inside the bowl. Droplets, waves, blobs and splashes are not
 * separate features — they all fall out of the same particles.
 *
 * The method is PBF (position based fluids): correct positions directly
 * rather than applying forces.
 *   1. move everything under gravity
 *   2. count neighbours to measure density — too dense and they push apart,
 *      too sparse and they pull together
 *   3. repeat that correction two or three times until the density is even
 *   4. recover velocity from how far things actually moved
 * Forces blow up as soon as the timestep grows slightly; correcting positions
 * does not. That is why real-time fluids are written this way.
 *
 * Cost: particles x about twelve neighbours x three passes, which is a few
 * hundred thousand operations a frame. At 240 MHz that fits. Neighbours are
 * found through a grid — each particle looks at its own cell and the eight
 * around it. Testing every pair would be N squared.
 *
 * 🚨 The picture is drawn by us into a 466x466 RGB565 buffer (434 KB, PSRAM)
 * and handed over as one image. Before that it hooked into LVGL's 16-line
 * bands and drew inside each one — thirty bands meant thirty callbacks a
 * frame, each walking 466 columns and calling lv_draw_rect more than twenty
 * thousand times. That was the 311 ms frame interval (3 fps), and those slow
 * frames stretched a physics step to 33 ms, which threw the water out of the
 * bowl entirely.
 *
 * 🚨 An older comment forbade this, calling a PSRAM canvas "the trap that
 * failed 13,943 draws". That trap is a different thing: there, LVGL's **draw
 * buffer** was in PSRAM, so SPI could not DMA from it and every transfer had
 * to allocate an internal bounce buffer. This image is not pushed to the
 * panel — it is the **source** the CPU reads. LVGL copies from here into a
 * 16-line internal RAM buffer and DMA runs from that. The DMA path is
 * unchanged. */
#include "app.h"
#include "port.h"
#include "water.h"
#include <math.h>
#include <string.h>
#ifdef BADGE_SIM
/* The simulator has neither PSRAM nor esp_timer. Stand-ins under the same names. */
#  include <stdlib.h>
#  include <stdio.h>
#  include <sys/time.h>
#  define MALLOC_CAP_SPIRAM 0
#  define MALLOC_CAP_INTERNAL 0
#  define MALLOC_CAP_8BIT 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void *heap_caps_calloc(size_t n, size_t sz, int caps) { (void)caps; return calloc(n, sz); }
static void  heap_caps_free(void *p) { free(p); }
static int64_t esp_timer_get_time(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
#  define ESP_LOGI(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#  define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#else
#  include "esp_heap_caps.h"
#  include "esp_timer.h"
#  include "esp_log.h"
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "freertos/semphr.h"
#endif

#define CX      233
#define CY      233
#define R       222         /* the circle the water sits in */
#define DEG2RAD 0.0174533f

/* ── particles ───────────────────────────────────────────────
 * Filling the half-circle with this many leaves them about 11 px apart. The
 * neighbour radius has to be twice that for them to feel each other properly
 * — too tight and it behaves like sand, not water. */
/* 🚨 The count was cut from 620 to 460 and then put back. The cut was made
 * because the physics was eating the frame, and the physics has since moved
 * to core 1 where it overlaps with drawing — it is no longer in the frame at
 * all. */
/* 🚨 The particle count *is* a physics parameter: cost grows with the square
 * of it, because more particles in the same bowl means more neighbours each.
 * Measured: 620 particles -> 27 ms per step, and real time needs eight steps,
 * i.e. 216 ms. The core-1 budget is one frame (80 ms), so at 620 only one or
 * two steps fit — which is what the slow motion was. At 380 a step is 9 ms
 * and all eight fit. Viscosity went up to 0.22 to keep the sense of body. */
#define NP      380
#define HR      22.0f                   /* neighbour radius */
#define INV_HR  (1.0f / HR)
#define HR2     (HR * HR)

/* 🚨 Declaring these static would hold 27 KB of internal RAM permanently,
 * whether or not the app is open — and there are only 116 KB of it. They are
 * allocated from PSRAM on entry and freed on exit.
 * PSRAM is fine here because this is data the CPU reads and writes, not a
 * buffer pushed to the panel — it has nothing to do with the DMA trap. */
static float *s_x, *s_y;                /* current positions — the physics writes these */
static float *s_px, *s_py;              /* positions before the move */
static float *s_vx, *s_vy;

/* 🚨 The copy the renderer reads. While the physics runs on core 1, core 0
 * reads this — reading the same array another core is writing either tears
 * the picture or crashes. The copy is taken at the one point where a step has
 * finished and nothing is touching them.
 * The simulator has one core, so it just points at the same arrays. */
static float *s_rx, *s_ry, *s_rvx, *s_rvy;

/* ── physics on core 1 ────────────────────────────────────────
 * 🚨 There are two cores and everything heavy was on core 0 (main and BLE
 * both). The physics has nothing to do with drawing, so moving it sideways
 * takes it out of the frame entirely — of a measured 110 ms frame, 37 ms was
 * particles.
 *
 * How they overlap: while core 0 draws step N, core 1 solves step N+1.
 *   1. wait for the previous step to finish (done)
 *   2. take the copy right there, while nothing is touching it
 *   3. hand over the conditions for the next step and signal go
 *   4. core 0 draws from the copy
 * 🚨 Reading the live arrays without the copy tears the picture.
 * 🚨 On the way out, confirm the step has finished before freeing the arrays.
 *    The other order has core 1 writing into memory that is already gone. */
static struct {
    float gx, gy, kx, ky, burst, sdt;
    int   sub;
} s_pin;
static volatile bool s_still_flag;

static void fluid_step(float dt, float gx, float gy);
/* 🚨 Insurance, not a fix. Dynamic frequency scaling parks the CPU at min_freq
 * when nobody holds a CPU_FREQ_MAX lock, which would leave an app whose
 * arithmetic is the frame running at a third speed. But measured on hardware
 * 09-12 the chip was already at 240 MHz before the water opened — something
 * inside IDF holds it — and taking the lock changed no frame time at all. It
 * earns its keep the day that something goes away. Recorded because it was
 * written on the assumption that the clock was low, and that assumption was
 * wrong. */
static bool s_fast_held;

static void water_fast(bool on)
{
    if (on == s_fast_held) return;
    s_fast_held = on;
    port_perf_hold(on);
}

static bool water_is_still(void);

/* One step. Does the same thing whichever core calls it. */
static volatile uint32_t s_phys_us;   /* how long the last step took */
static volatile int      s_phys_sub;  /* and how many sub-steps it ran */

static void phys_round(void)
{
    int64_t _p0 = esp_timer_get_time();
    if (s_pin.kx || s_pin.ky || s_pin.burst) {
        static uint32_t seed = 987654321u;
        for (int i = 0; i < NP; i++) {
            s_vx[i] += s_pin.kx;
            s_vy[i] += s_pin.ky;
            if (s_pin.burst > 0) {
                /* A dead-centre poke has no direction — scatter each one somewhere */
                seed = seed * 1103515245u + 12345u;
                float a = (float)((seed >> 9) & 0xFFFF) / 65535.0f * 6.2831853f;
                s_vx[i] += cosf(a) * s_pin.burst;
                s_vy[i] += sinf(a) * s_pin.burst;
            }
        }
    }
    for (int k = 0; k < s_pin.sub; k++) fluid_step(s_pin.sdt, s_pin.gx, s_pin.gy);
    s_still_flag = water_is_still();
    s_phys_us  = (uint32_t)(esp_timer_get_time() - _p0);
    s_phys_sub = s_pin.sub;
}

#ifndef BADGE_SIM
static SemaphoreHandle_t s_go, s_done;
static TaskHandle_t      s_phys;
static bool              s_inflight;

static void phys_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        phys_round();
        xSemaphoreGive(s_done);
    }
}

/* Wait for the running step to finish. Called first on the way out, too. */
static void phys_wait(void)
{
    if (!s_inflight) return;
    /* 🚨 Wait generously, but not forever. A stuck core 1 would freeze the display. */
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    s_inflight = false;
}
#else
static void phys_wait(void) { }
#endif

/* The neighbour grid — each particle looks at its own cell and the eight around it */
#define GC      24                      /* cells per side */
#define GS      (466.0f / GC)            /* cell size (about 19 px) */
#define INV_GS  ((float)GC / 466.0f)     /* multiply, never divide — see frsqrt above */
static uint16_t s_head[GC * GC];        /* first particle per cell (0xFFFF = empty) — 1.1 KB, kept static */
static uint16_t *s_next;                /* next particle in the same cell */

static lv_obj_t   *s_root, *s_field, *s_hint;
static lv_timer_t *s_loop;
static float s_gx, s_gy;                /* which way gravity points, in screen coordinates */
static float s_boat_x, s_boat_vx;
static float s_boat_y, s_boat_vy;
/* Unit vector along gravity. Water, boat and renderer all have to agree on "down". */
static float s_gux = 0.0f, s_guy = 1.0f;

/* Screen coordinates <-> a frame where gravity is down. This is what lets the
 * boat ride at an angle. A = along gravity (down is +), C = perpendicular to
 * it. With gravity straight down the screen, they are the identity. */
#define G_ALONG(px, py)  (((px) - CX) * s_gux + ((py) - CY) * s_guy)
#define G_CROSS(px, py)  (((px) - CX) * s_guy - ((py) - CY) * s_gux)
static float s_fake_gx, s_fake_gy;
static uint32_t s_last_ms;

/* ── the neighbour grid ────────────────────────────────────
 * Testing every pair is N squared. Split the area into cells and look only at
 * your own cell and the eight around it. */
static void grid_build_from_current(void)
{
    memset(s_head, 0xFF, sizeof s_head);
    for (int i = 0; i < NP; i++) {
        int cx = (int)(s_x[i] * INV_GS), cy = (int)(s_y[i] * INV_GS);
        if (cx < 0) cx = 0;
        if (cx >= GC) cx = GC - 1;
        if (cy < 0) cy = 0;
        if (cy >= GC) cy = GC - 1;
        int c = cy * GC + cx;
        s_next[i] = s_head[c];
        s_head[c] = (uint16_t)i;
    }
}

/* Put it back inside the bowl */
/* 🚨 This chip has no divide or square-root hardware. `a / b`, `a / 6.0f` and
 * sqrtf all compile to a library call (__divsf3, sqrtf) — counted in the
 * simulator, one frame did 33,074 divides in the density loop alone and 37,653
 * sqrt-plus-divides in the neighbour loop. So:
 *   1) every divide by a constant becomes a multiply by its reciprocal (INV_*)
 *   2) 1/sqrt gets an approximation that calls nothing
 *
 * Two Newton steps leave about 5e-6 of relative error, which physics at this
 * scale cannot feel. Every caller guards its input away from zero first. */
static inline float frsqrt(float x)
{
    union { float f; uint32_t u; } c;
    float xh = 0.5f * x;
    c.f = x;
    c.u = 0x5f3759dfu - (c.u >> 1);   /* halve the exponent for a first guess */
    float y = c.f;
    y = y * (1.5f - xh * y * y);
    y = y * (1.5f - xh * y * y);
    return y;
}

static inline void clamp_circle(float *x, float *y)
{
    float dx = *x - CX, dy = *y - CY;
    float d2 = dx * dx + dy * dy;
    if (d2 > (float)(R - 2) * (R - 2)) {
        float k = (R - 2) * frsqrt(d2);
        *x = CX + dx * k;
        *y = CY + dy * k;
    }
}

/* ── one water step ────────────────────────────────────────
 * 🚨 This was first written with the textbook PBF kernels (poly6/spiky), but
 * the relaxation constant's scale did not match our units (pixels) and the
 * correction came out as effectively zero — the particles settled into a thin
 * layer on the bottom. Getting those constants right is fiddly.
 *
 * So it uses double density relaxation (Clavet) instead. That was designed
 * for games: it is insensitive to scale and it does not blow up. Two
 * densities are tracked:
 *   rho      ordinary density — denser than the rest pushes apart, sparser
 *            pulls together (this is the surface tension)
 *   rho_near counts only very close neighbours and only ever pushes
 * The second one is what stops particles collapsing onto a single point. */
#define KSTIFF   26000.0f       /* strength of the ordinary density term */
#define KNEAR    78000.0f       /* strength of the near term — prevents overlap */
static float s_rho0;            /* the density when it is at rest */

/* 🚨 The neighbours were being walked twice: once to count density, once to
 * push using it. But nothing moves in between — this particle's own
 * correction (dxi) is added once at the end, and each neighbour j is touched
 * once per particle. So the dx, dy and r the second pass recomputes are
 * **literally the same numbers** as the first pass.
 * They are stored during the first walk and the second pass just reads them.
 * A particle sees about twenty neighbours, so storing them is cheap. It
 * halves the sqrtf calls and halves the grid walking.
 * The maths is unchanged — verified by checking that 400 frames in the
 * simulator produce an identical fingerprint. */
#define NB_MAX 128        /* most neighbours one particle will track; it says so if exceeded */

static void neighbors_relax(float dt)
{
    /* 🚨 File statics, not stack. Only the one core-1 task runs the physics,
     * so it is safe, and it keeps 1.8 KB off that task's stack. */
    static uint16_t nb_j[NB_MAX];
    static float    nb_q[NB_MAX], nb_ux[NB_MAX], nb_uy[NB_MAX];

    float dt2 = dt * dt;
    for (int i = 0; i < NP; i++) {
        float rho = 0, rhon = 0;
        int cx = (int)(s_x[i] * INV_GS), cy = (int)(s_y[i] * INV_GS);
        int nb = 0;

        /* ── first walk: count density and collect the neighbours ── */
        for (int oy = -1; oy <= 1; oy++) {
            int yy = cy + oy;
            if (yy < 0 || yy >= GC) continue;
            for (int ox = -1; ox <= 1; ox++) {
                int xx = cx + ox;
                if (xx < 0 || xx >= GC) continue;
                for (uint16_t j = s_head[yy * GC + xx]; j != 0xFFFF; j = s_next[j]) {
                    if (j == i) continue;
                    float dx = s_x[j] - s_x[i], dy = s_y[j] - s_y[i];
                    float r2 = dx * dx + dy * dy;
                    if (r2 >= HR2 || r2 < 1e-4f) continue;
                    /* 🚨 Division is about twenty cycles on Xtensa. HR is
                     * constant so it becomes a multiply, and 1/r is computed
                     * once and used for both axes. */
                    float inv_r = frsqrt(r2);
                    float r = r2 * inv_r;
                    float q = 1.0f - r * INV_HR;
                    rho  += q * q;
                    rhon += q * q * q;
                    if (nb < NB_MAX) {
                        nb_j[nb]  = j;
                        nb_q[nb]  = q;
                        nb_ux[nb] = dx * inv_r;
                        nb_uy[nb] = dy * inv_r;
                        nb++;
                    } else {
                        /* 🚨 Reaching here means more than 128 particles in
                         * one spot — a third of them inside a radius of 22,
                         * which does not actually happen. If it ever does,
                         * we want to know. */
                        static bool told;
                        if (!told) { told = true; ESP_LOGW("water", "more than %d neighbours", NB_MAX); }
                    }
                }
            }
        }

        /* ── second walk: push, using what was collected ─────── */
        float P  = KSTIFF * (rho - s_rho0);
        float PN = KNEAR  * rhon;
        float dxi = 0, dyi = 0;
        for (int k = 0; k < nb; k++) {
            float q = nb_q[k];
            float d = dt2 * (P * q + PN * q * q) * 0.5f;
            float ux = nb_ux[k], uy = nb_uy[k];
            uint16_t j = nb_j[k];
            s_x[j] += ux * d;  s_y[j] += uy * d;
            dxi    -= ux * d;  dyi    -= uy * d;
        }
        s_x[i] += dxi; s_y[i] += dyi;
    }
}

static void fluid_step(float dt, float gx, float gy)
{
    /* Viscosity — share a little of the speed at which neighbours approach or
     * separate. This is what makes the water flow as one body instead of
     * breaking into pieces. */
    grid_build_from_current();
    for (int i = 0; i < NP; i++) {
        int cx = (int)(s_x[i] * INV_GS), cy = (int)(s_y[i] * INV_GS);
        for (int oy = -1; oy <= 1; oy++) {
            int yy = cy + oy;
            if (yy < 0 || yy >= GC) continue;
            for (int ox = -1; ox <= 1; ox++) {
                int xx = cx + ox;
                if (xx < 0 || xx >= GC) continue;
                for (uint16_t j = s_head[yy * GC + xx]; j != 0xFFFF; j = s_next[j]) {
                    if (j <= i) continue;             /* each pair once */
                    float dx = s_x[j] - s_x[i], dy = s_y[j] - s_y[i];
                    float r2 = dx * dx + dy * dy;
                    if (r2 >= HR2 || r2 < 1e-4f) continue;
                    float inv_r = frsqrt(r2);
                    float ux = dx * inv_r, uy = dy * inv_r;
                    float vr = (s_vx[i] - s_vx[j]) * ux + (s_vy[i] - s_vy[j]) * uy;
                    if (vr <= 0) continue;            /* only when approaching */
                    float q = 1.0f - (r2 * inv_r) * INV_HR;
                    /* 🚨 Viscosity is the sense of weight. At 0.10 the
                     * particles barely hold onto each other and scatter like
                     * sand ("the water moves too lightly to be water").
                     * Higher and it flows as one sticky body — and as a
                     * bonus it settles sooner, so water_is_still() triggers
                     * more often and the drawing costs less. */
                    /* 🚨 0.10 -> 0.22 -> 0.32. ("It feels like gravity is too
                     * strong. Give it some weight.") Lowering gravity alone
                     * does not read as heavy — it reads as floating
                     * underwater. Viscosity has to come up with it. */
                    float imp = dt * q * (0.55f * vr) * 0.5f;
                    s_vx[i] -= ux * imp; s_vy[i] -= uy * imp;
                    s_vx[j] += ux * imp; s_vy[j] += uy * imp;
                }
            }
        }
    }

    /* Move under gravity */
    for (int i = 0; i < NP; i++) {
        s_vx[i] += gx * dt;
        s_vy[i] += gy * dt;
        s_px[i] = s_x[i];                 /* remember where it was */
        s_py[i] = s_y[i];
        s_x[i] += s_vx[i] * dt;
        s_y[i] += s_vy[i] * dt;
    }

    /* Even out the density */
    grid_build_from_current();
    neighbors_relax(dt);

    /* Put them back in the bowl, and recover velocity from how far they moved */
    float inv = 1.0f / dt;
    for (int i = 0; i < NP; i++) {
        clamp_circle(&s_x[i], &s_y[i]);
        s_vx[i] = (s_x[i] - s_px[i]) * inv;
        s_vy[i] = (s_y[i] - s_py[i]) * inv;
        float sp2 = s_vx[i] * s_vx[i] + s_vy[i] * s_vy[i];
        /* 🚨 Lowering this to 1000 was a mistake. The cap has to sit **above
         * free-fall speed** — below it, water that is falling correctly gets
         * clipped and the whole thing looks slower and lighter. The bowl is
         * 400 tall and gravity is 1900, so sqrt(2gh) is about 1233, which
         * makes 1300 the floor. This is a net for runaway values, not a
         * weight control. */
        if (sp2 > 1300.0f * 1300.0f) {    /* too fast and it jumps past its neighbours and tears */
            float k = 1300.0f * frsqrt(sp2);
            s_vx[i] *= k; s_vy[i] *= k;
        }
    }
}

/* Score a shake smoothly. Just over the threshold barely counts, well over
 * counts fully — joined at zero so the value does not jump at the threshold. */
static float shake_curve(float v)
{
    float a = fabsf(v);
    if (a < 170.0f) return 0.0f;
    a -= 170.0f;
    float t = a / 600.0f;
    if (t > 1.0f) t = 1.0f;
    return (v < 0.0f ? -1.0f : 1.0f) * a * t;
}

/* ── drawing ──────────────────────────────────────────────────
 * Drawing the particles as circles looks like a heap of grains. Instead each
 * column is scanned for "where does water start and stop here?" and painted
 * as connected runs — touching particles become one body, and anything that
 * has come away is drawn separately. */
#define COLW 6                          /* width of a scan column */
#define NCOL (466 / COLW + 1)
#define ROWH 6
#define INV_COLW (1.0f / COLW)
#define INV_ROWH (1.0f / ROWH)
#define NROW (466 / ROWH + 1)
static uint8_t (*s_dens)[NCOL];
static uint8_t (*s_chur)[NCOL];   /* how agitated the water is there — drives the foam */

/* Runs of water per column. Up to three, so a detached blob is caught separately. */
/* 🚨 Three was not enough. A few specks of spray floating above the water
 * filled the list, and the body underneath was then never scanned at all —
 * a black vertical stripe down the column (2,394 of them in 400 simulator
 * frames). Six, and when even that runs out the last span swallows the rest
 * instead of the scan giving up. */
#define SPANS 6
static int16_t (*s_sp0)[SPANS], (*s_sp1)[SPANS];
static uint8_t *s_spn;
static uint8_t *s_sfrac;    /* what fraction of the pixel the surface covers — softens the edge */
static int8_t  *s_slope;    /* surface slope — lights the faces the light reaches */
static uint8_t *s_foam;     /* foam */
static int      *s_cur;     /* which run each column is on, while scanning a row */
static uint16_t *s_scol;    /* three precomputed surface colours per column */

/* 🚨 We fill the water image ourselves and hand it over as one picture; why
 * is in the comment above draw_cb. 466x466 RGB565 = 434 KB, and there are
 * 3.3 MB of PSRAM sitting idle. */
static uint16_t *s_img;
static lv_image_dsc_t s_img_dsc;
/* Where the water was last frame. Clearing only that avoids touching all
 * 434 KB. 🚨 draw_cb comes before paint_img, so the declaration lives here. */
static int s_img_y0 = 0, s_img_y1 = 465;
/* 🚨 Per column, the vertical range the water covered last frame. Clearing is
 * cut down to this — paint_img()'s clearing comment says why. p0 > p1 means
 * the column was empty. */
static int16_t *s_pv0, *s_pv1;
/* The rectangle the boat took last frame. It sails above the water too, so it
 * has to be remembered separately. */
static int s_bx0 = 1, s_bx1 = 0, s_by0 = 1, s_by1 = 0;
static lv_image_dsc_t s_img_strip;
static uint16_t s_depth_lut[201];   /* depth below the surface, in pixels -> colour */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static void depth_lut_build(void)
{
    for (int d = 0; d <= 200; d++) {
        float t = d / 200.0f;
        s_depth_lut[d] = RGB565((int)(0x6E + (0x0A - 0x6E) * t),
                                (int)(0xC4 + (0x30 - 0xC4) * t),
                                (int)(0xE8 + (0x58 - 0xE8) * t));
    }
}

/* How thick the water is there — read with interpolation between cells, or it looks faceted */
static float dens_at(float fx, int r)
{
    if (r < 0 || r >= NROW) return 0;
    float c = fx * INV_COLW;
    int   ci = (int)c;
    float t = c - ci;
    if (ci < 0) { ci = 0; t = 0; }
    if (ci >= NCOL - 1) { ci = NCOL - 2; t = 1; }
    return s_dens[r][ci] * (1 - t) + s_dens[r][ci + 1] * t;
}

/* 🚨 Painting the 6 px cells as they are gives visible stair steps. Finding
 * where the thickness crosses the threshold, interpolated between cells,
 * makes it smooth. Computed once a frame and only read per band. */
#define THRESH 3.0f
static void build_spans(void)
{
    memset(s_dens, 0, (size_t)NROW * NCOL);
    memset(s_chur, 0, (size_t)NROW * NCOL);
    for (int i = 0; i < NP; i++) {
        int c = (int)(s_rx[i] * INV_COLW), r = (int)(s_ry[i] * INV_ROWH);
        /* Where the fast particles gather is where it breaks — that is where foam appears */
        float sp = fabsf(s_rvx[i]) + fabsf(s_rvy[i]);
        int ch = (int)(sp * 0.35f);
        if (ch > 60) ch = 60;
        for (int dr = -2; dr <= 2; dr++) {
            int rr = r + dr;
            if (rr < 0 || rr >= NROW) continue;
            for (int dc = -2; dc <= 2; dc++) {
                int cc = c + dc;
                if (cc < 0 || cc >= NCOL) continue;
                int w = 5 - (dr * dr + dc * dc);
                if (w <= 0) continue;
                int v = s_dens[rr][cc] + w;
                s_dens[rr][cc] = v > 255 ? 255 : (uint8_t)v;
                int u = s_chur[rr][cc] + ((ch * w) >> 2);
                s_chur[rr][cc] = u > 255 ? 255 : (uint8_t)u;
            }
        }
    }
    for (int x = 0; x < 466; x++) {
        int n = 0;
        float prev = dens_at(x, 0);
        int   topr = -1; float topf = 0;
        /* 🚨 This used to stop scanning once the list was full (n == SPANS),
         * which is what produced the black stripes. Scan to the bottom always;
         * if there is no room left, stretch the last span to swallow it. */
        for (int r = 1; r < NROW; r++) {
            float d = dens_at(x, r);
            bool was = prev >= THRESH, now = d >= THRESH;
            if (!was && now) {                      /* water starts here */
                float f = (THRESH - prev) / (d - prev + 1e-6f);
                topr = r - 1; topf = f;
                if (n == 0) {
                    /* What fraction of that pixel the surface covers. Blending
                     * the top row with it hides the jaggedness (sub-pixel). */
                    float yf = (topr + topf) * ROWH;
                    s_sfrac[x] = (uint8_t)((1.0f - (yf - floorf(yf))) * 255.0f);
                }
            } else if (was && !now && topr >= 0) {   /* and ends here */
                float f = (prev - THRESH) / (prev - d + 1e-6f);
                int16_t y0 = (int16_t)((topr + topf) * ROWH);
                int16_t y1 = (int16_t)((r - 1 + f) * ROWH);
                if (y1 > y0) {
                    if (n < SPANS) { s_sp0[x][n] = y0; s_sp1[x][n] = y1; n++; }
                    else           { s_sp1[x][SPANS - 1] = y1; }
                }
                topr = -1;
            }
            prev = d;
        }
        if (topr >= 0) {                             /* water all the way down */
            if (n < SPANS) {
                s_sp0[x][n] = (int16_t)((topr + topf) * ROWH);
                s_sp1[x][n] = 465;
                n++;
            } else {
                s_sp1[x][SPANS - 1] = 465;
            }
        }
        s_spn[x] = (uint8_t)n;

        /* How agitated the water is there — measured just under the surface */
        int fr = 0;
        if (n) {
            int rr = s_sp0[x][0] / ROWH;
            if (rr >= 0 && rr < NROW) {
                int cc = x / COLW;
                if (cc >= NCOL) cc = NCOL - 1;
                fr = s_chur[rr][cc];
            }
        }
        s_foam[x] = (uint8_t)fr;
    }

    /* Surface slope. Without it, lighting the faces is impossible and the
     * picture looks flat however well the water moves. */
    for (int x = 0; x < 466; x++) {
        int a = x > 3 ? x - 4 : 0, b = x < 462 ? x + 4 : 465;
        if (!s_spn[a] || !s_spn[b]) { s_slope[x] = 0; continue; }
        int d = (s_sp0[b][0] - s_sp0[a][0]) * 8 / (b - a);
        if (d > 127) d = 127;
        if (d < -127) d = -127;
        s_slope[x] = (int8_t)d;
    }
}

/* 🚨 Why we paint the water ourselves — the measurements answer it.
 *
 * This callback used to call lv_draw_rect per column: ten depth bands plus
 * three surface rows is thirteen per column, so 6,000 for 466 columns. In
 * practice it was far more than that. display.c's draw buffer is sixteen
 * lines (SPI DMA wants that much internal RAM, and the internal heap bottoms
 * out at 23 KB while this app runs), so LVGL splits a frame into 466/16 = 30
 * pieces and calls this back for each one, walking all 466 columns every
 * time. That is more than twenty thousand draw_rect calls a frame.
 *
 * Measured: 39 ms of computation, a 311 ms real frame interval — 3 fps. The
 * missing 272 ms was all here. And those slow frames stretched a physics step
 * to 33 ms (it should be 8), which threw the water out of the bowl.
 *
 * Reducing it to one vertical gradient per column was tried first and came
 * out *slower* (311 -> 450 ms): every column has different colours, so that
 * is 466 distinct gradients, and LVGL builds a fresh map for each one.
 *
 * So instead of calling less, it does not call at all. paint_img() writes the
 * pixels into our own buffer once (independently of how LVGL splits the
 * frame) and this hands over a single image — one call per piece, thirty per
 * frame. Twenty thousand became thirty.
 *
 * 🚨 The boat is drawn after the water here. It used to be drawn first and
 * covered by the water so the submerged part was hidden, but the image is
 * opaque and in that order the boat is simply erased. If the boat needs to
 * sit in the water again, it has to be painted inside paint_img(). */
/* 🚨 LVGL paints the invalidated area with the screen background (black) and
 * then draws our image over it — even though our image covers that area
 * opaquely. That is every pixel touched twice. Telling it we cover the area
 * removes the background pass entirely (it was one of two passes in 75 ms of
 * drawing).
 * 🚨 Only claim to cover inside the band we actually painted (s_img_y0..y1).
 * Claiming more than that leaves things that should have been erased. */
static void cover_cb(lv_event_t *e)
{
    if (!s_img) { lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER); return; }
    const lv_area_t *a = lv_event_get_cover_area(e);
    if (a->y1 >= s_img_y0 && a->y2 <= s_img_y1 && a->x1 >= 0 && a->x2 <= 465)
        lv_event_set_cover_res(e, LV_COVER_RES_COVER);
    else
        lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER);
}

static void draw_cb(lv_event_t *e)
{
    if (!s_x || !s_spn) return;      /* already freed — do not draw */
    lv_layer_t *layer = lv_event_get_layer(e);

    if (s_img) {
        /* 🚨 Hand over only the band that has water in it. LVGL repaints the
         * background elsewhere anyway, so the rest stays black — there is
         * nothing to send. */
        lv_draw_image_dsc_t idsc;
        lv_draw_image_dsc_init(&idsc);
        /* A separate descriptor points at the band's first row. Changing the
         * start address and the height is less error-prone than cropping. */
        idsc.src = &s_img_strip;
        lv_area_t coords = { 0, s_img_y0, 465, s_img_y1 };
        lv_draw_image(layer, &idsc, &coords);
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    (void)dsc;

}

/* ── painting the boat into the image ─────────────────────────
 * 🚨 The boat used to be an LVGL triangle drawn **before** the water, so the
 * submerged part was covered. Switching to handing over one image pushed the
 * boat behind the water — and since the image is opaque, the boat ended up
 * always on top instead ("it never goes under the water"). Painting the boat
 * into the same image restores the ordering. As a bonus, twelve triangles
 * times twenty pieces — 240 draw calls — disappear. */
static void fill_tri(float x0, float y0, float x1, float y1,
                     float x2, float y2, uint16_t c)
{
    int ymin = (int)floorf(fminf(y0, fminf(y1, y2)));
    int ymax = (int)ceilf (fmaxf(y0, fmaxf(y1, y2)));
    if (ymin < 0) ymin = 0;
    if (ymax > 465) ymax = 465;
    for (int y = ymin; y <= ymax; y++) {
        float yc = y + 0.5f;
        /* Collect the x values where the three edges cross this row */
        float xs[3]; int nx = 0;
        float px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
        for (int e = 0; e < 3; e++) {
            int f = (e + 1) % 3;
            float ya = py[e], yb = py[f];
            if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya)) {
                float t = (yc - ya) / (yb - ya);
                xs[nx++] = px[e] + (px[f] - px[e]) * t;
            }
        }
        if (nx < 2) continue;
        float xa = xs[0], xb = xs[1];
        if (xa > xb) { float t = xa; xa = xb; xb = t; }
        int ia = (int)ceilf(xa - 0.5f), ib = (int)floorf(xb - 0.5f);
        if (ia < 0) ia = 0;
        if (ib > 465) ib = 465;
        uint16_t *row = s_img + (size_t)y * 466;
        for (int x = ia; x <= ib; x++) row[x] = c;
    }
}

/* Paint the boat at its current attitude and return the rows it occupied. */
static void paint_boat(int *out_x0, int *out_x1, int *out_y0, int *out_y1)
{
    float bx = s_boat_x, by = s_boat_y;
    float bdeg = 0;
    {   /* Lean it with the slope of the water beneath it */
        float lft = 0, rgt = 0; int nl = 0, nr = 0;
        for (int i = 0; i < NP; i++) {
            float d = s_rx[i] - bx;
            if (fabsf(d) > 40 || s_ry[i] > by + 40) continue;
            if (d < 0) { lft += s_ry[i]; nl++; } else { rgt += s_ry[i]; nr++; }
        }
        if (nl && nr) bdeg = atanf(((rgt / nr) - (lft / nl)) / 40.0f) / DEG2RAD * 0.6f;
    }
    /* 🚨 Stand it up along gravity. With the sign inverted, the boat was
     * upside down at 90 and 270 degrees only and its mast went into the water
     * — at 0 and 180 an inverted sign gives the same angle, so it looked half
     * correct. */
    bdeg -= atan2f(s_gux, s_guy) / DEG2RAD;

    /* 🚨 This angle used to be used directly, and the boat snapped round the
     * instant you tilted. A heavy boat cannot turn like that, so it follows
     * behind.
     * 🚨 Angles wrap at ±180, so the difference has to be folded into that
     * range first. Without folding it takes the long way round. */
    {
        static float sm;
        static bool  first = true;
        if (first) { sm = bdeg; first = false; }
        float d = bdeg - sm;
        while (d >  180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        sm += d * 0.11f;
        bdeg = sm;
    }
    float bc = cosf(bdeg * DEG2RAD), bs = sinf(bdeg * DEG2RAD);

    int lo = 465, hi = 0, xlo = 465, xhi = 0;
    #define BX(px, py) (bx + (px) * bc - (py) * bs)
    #define BY(px, py) (by + (px) * bs + (py) * bc)
    #define TRI(c, ax, ay, bx_, by_, cx, cy) do {         float _y0 = BY(ax, ay), _y1 = BY(bx_, by_), _y2 = BY(cx, cy);         float _x0 = BX(ax, ay), _x1 = BX(bx_, by_), _x2 = BX(cx, cy);         fill_tri(_x0, _y0, _x1, _y1, _x2, _y2, (c));         float _lo = fminf(_y0, fminf(_y1, _y2)), _hi = fmaxf(_y0, fmaxf(_y1, _y2));         if ((int)_lo < lo) lo = (int)_lo;         if ((int)_hi + 1 > hi) hi = (int)_hi + 1;         float _xl = fminf(_x0, fminf(_x1, _x2)), _xh = fmaxf(_x0, fmaxf(_x1, _x2));         if ((int)_xl < xlo) xlo = (int)_xl;         if ((int)_xh + 1 > xhi) xhi = (int)_xh + 1;     } while (0)
    /* 🚨 A dark silhouette disappeared against deep water. The hull is lifted
     * to a wood tone and the outline is a shade brighter still. */
    const uint16_t HULL  = RGB565(0xB0, 0x70, 0x3C), HULL2 = RGB565(0xE0, 0xA4, 0x68);
    const uint16_t SAIL  = RGB565(0xFF, 0xFD, 0xF6), SAIL2 = RGB565(0xE8, 0xDC, 0xC4);
    const uint16_t FLAG  = RGB565(0xFF, 0xE0, 0x8A);
    TRI(HULL,  -30,-7,  -14,-11, -16, 6);
    TRI(HULL,  -14,-11,  10,-11, -16, 6);
    TRI(HULL,   10,-11,  14,  6, -16, 6);
    TRI(HULL,   10,-11,  33, -9,  14, 6);
    TRI(HULL2, -30,-7,   33, -9, -14,-11);
    TRI(HULL2,  -2,-52,   1,-52,   3,-11);
    TRI(HULL2,   1,-52,   3,-11,   0,-11);
    TRI(SAIL,    3,-50,  22,-24,   3,-13);
    TRI(SAIL,   22,-24,  26,-15,   3,-13);
    TRI(SAIL2,   3,-50,   9,-31,   3,-13);
    TRI(SAIL2,  -2,-46, -19,-13,  -2,-13);
    TRI(FLAG,   -6,-14,  -3,-14,  -4,-18);
    #undef TRI
    #undef BY
    #undef BX
    if (lo < 0) lo = 0;
    if (hi > 465) hi = 465;
    if (xlo < 0) xlo = 0;
    if (xhi > 465) xhi = 465;
    *out_x0 = xlo;  *out_x1 = xhi;
    *out_y0 = lo;
    *out_y1 = hi;
}

#ifdef BADGE_SIM
/* ── checking that the clearing is right (simulator only) ────
 * Dropping the memset for "clear only what changed" means one missed spot
 * leaves a pixel that should have gone — and that is invisible to the eye in a
 * picture of sloshing water. So the meaning is checked directly: **any pixel
 * that is not black and is outside what was painted this frame is last frame's
 * leftover.**
 *   WATER_VERIFY=1 ./badge_sim --serve
 * turns it on. It is slow, so it is off by default. */
static void paint_verify(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("WATER_VERIFY"); on = (e && *e != '0'); }
    if (!on) return;
    int bad = 0, bx = 0, by = 0;
    for (int x = 0; x < 466 && !bad; x++) {
        for (int y = 0; y < 466; y++) {
            if (!s_img[(size_t)y * 466 + x]) continue;
            if (x >= s_bx0 && x <= s_bx1 && y >= s_by0 && y <= s_by1) continue;
            bool in = false;
            for (int k = 0; k < s_spn[x]; k++)
                if (y >= s_sp0[x][k] && y <= s_sp1[x][k]) { in = true; break; }
            if (!in) { bad++; bx = x; by = y; break; }
        }
    }
    if (bad) ESP_LOGW("water", "leftover pixel (%d,%d) — the clearing missed a spot", bx, by);
}
#endif

/* ── filling in the water image ───────────────────────────────
 * The runs (s_sp0/s_sp1) were already worked out by step(). This only writes
 * pixels. It runs once regardless of how LVGL splits the frame — which is the
 * entire point of doing it this way. */
static void paint_img(void)
{
    if (!s_img) return;
    /* 🚨 Clearing the band the water was in was already an improvement over
     * 434 KB, and it is still far too much. This cache is write-allocate: a
     * store to a line that is not resident *reads* it from PSRAM first. So a
     * memset over 300 rows reads 280 KB, writes 280 KB, and then the water is
     * painted over the top of it — the whole cost of touching the same place
     * twice is waste.
     *
     * So there is no memset at all. Only what actually changed is cleared:
     *   - per column, where the water was last frame minus where it is now
     *   - the gaps between this frame's runs (between separated droplets)
     *   - the rectangle the boat was in last frame
     * The runs themselves are repainted in full every frame, so clearing those
     * three keeps the picture exact. The surface moves a few pixels a frame, so
     * the clearing drops from 280 KB to a few KB.
     *
     * 🚨 s_img therefore has to be calloc'd — with no memset, anywhere that is
     * never painted goes out with whatever it was allocated with. */
    int ny0 = 465, ny1 = 0;

    /* Clear the rectangle the boat was in. Anywhere the water covered gets
     * repainted below anyway. */
    if (s_bx0 <= s_bx1 && s_by0 <= s_by1) {
        size_t w = (size_t)(s_bx1 - s_bx0 + 1) * 2;
        for (int y = s_by0; y <= s_by1; y++)
            memset(s_img + (size_t)y * 466 + s_bx0, 0, w);
        if (s_by0 < ny0) ny0 = s_by0;
        if (s_by1 > ny1) ny1 = s_by1;
    }

    /* Per column, clear where the water has left. Inside this frame's runs is
     * painted over shortly, so it is left alone. */
    for (int x = 0; x < 466; x++) {
        int p0 = s_pv0[x], p1 = s_pv1[x];
        int nsp = s_spn[x];
        int n0 = nsp ? s_sp0[x][0] : 1, n1 = nsp ? s_sp1[x][nsp - 1] : 0;
        if (p0 <= p1) {
            int a1 = p1 < n0 - 1 ? p1 : n0 - 1;        /* above the new water */
            for (int y = p0; y <= a1; y++) s_img[(size_t)y * 466 + x] = 0;
            int b0 = p0 > n1 + 1 ? p0 : n1 + 1;        /* below it */
            for (int y = b0; y <= p1; y++) s_img[(size_t)y * 466 + x] = 0;
            if (p0 < ny0) ny0 = p0;
            if (p1 > ny1) ny1 = p1;
        }
        /* The gaps between broken-off blobs — the painting side never goes there */
        for (int k = 1; k < nsp; k++) {
            int g0 = s_sp1[x][k - 1] + 1, g1 = s_sp0[x][k] - 1;
            for (int y = g0; y <= g1; y++) s_img[(size_t)y * 466 + x] = 0;
        }
        s_pv0[x] = (int16_t)n0;
        s_pv1[x] = (int16_t)n1;
    }

    /* 🚨 Paint the boat **before** the water, so the water covers the
     * submerged part. With that order reversed the boat always floated on top. */
    {
        int b0, b1;
        int bxa, bxb;
        paint_boat(&bxa, &bxb, &b0, &b1);
        if (b0 < ny0) ny0 = b0;
        if (b1 > ny1) ny1 = b1;
        s_bx0 = bxa; s_bx1 = bxb; s_by0 = b0; s_by1 = b1;
    }
    /* 🚨 This used to loop columns first and walk down inside each one. The
     * image buffer is laid out in rows (932 bytes each), so walking down
     * jumps the address by 932 bytes per pixel — two hundred thousand writes,
     * every one on a different cache line, into PSRAM. Filling the image
     * alone cost 35 ms.
     * Flipped to rows, writing a row contiguously, so the writes burst.
     *
     * Everything a column needs (three surface colours, the depth reference)
     * is computed up front — the inner loop over the row stays as light as
     * possible. */
    int wy0 = 465, wy1 = 0;
    for (int x = 0; x < 466; x++) {
        s_cur[x] = 0;
        int nsp = s_spn[x];
        if (!nsp) continue;
        int s0 = s_sp0[x][0];
        if (s0 < wy0) wy0 = s0;
        int last = s_sp1[x][nsp - 1];
        if (last > wy1) wy1 = last;

        /* ── the three surface rows ─────────────────────────
         * 🚨 Flat colour fields look flat however well the water moves.
         * Three things make it read as water:
         *   slope     faces toward the light are bright, away are dark
         *   foam      where it breaks, it goes white
         *   coverage  the top row is painted by how much it covers, which
         *             removes the jaggedness */
        int sl = s_slope[x];                 /* + means falling to the right */
        int lit = 128 - sl * 2;              /* light comes from the upper left */
        if (lit < 40) lit = 40;
        if (lit > 255) lit = 255;
        int fo = s_foam[x];
        for (int q = 0; q < 3; q++) {
            int br = (0x9A * lit) >> 8, bg = (0xD6 * lit) >> 8, bb2 = (0xF2 * lit) >> 8;
            if (q) { br = br * 3 / 4; bg = bg * 3 / 4; bb2 = bb2 * 3 / 4; }
            if (fo) {
                int f = fo > 200 ? 200 : fo;
                br += (255 - br) * f / 255;
                bg += (255 - bg) * f / 255;
                bb2 += (255 - bb2) * f / 255;
            }
            if (q == 0) {
                int cov = s_sfrac[x];
                br = br * cov / 255; bg = bg * cov / 255; bb2 = bb2 * cov / 255;
            }
            if (br > 255) br = 255;
            if (bg > 255) bg = 255;
            if (bb2 > 255) bb2 = 255;
            s_scol[x * 3 + q] = RGB565(br, bg, bb2);
        }
    }
    if (wy0 < 0) wy0 = 0;
    if (wy1 > 465) wy1 = 465;

    /* 🚨 How this is walked decides what it costs. Two wrong answers before
     * the third.
     *   1) columns outer, walking down — 932 bytes per pixel of jump. Going
     *      down one column touching 200 rows touches 200 cache lines, and the
     *      working set is 186 KB against a 64 KB cache. 35 ms.
     *   2) rows outer, walking across — the writes are contiguous, but every
     *      column has to be visited whether it has water or not, so the
     *      number of iterations went from 87,000 to 210,000. 65 ms. Worse.
     *   3) **walk down, but in bands of 64 rows** — one band's working set is
     *      64 x 932 = 59 KB, which fits in cache. It touches only pixels with
     *      water in them and stays cache-resident. The good half of both. */
    #define BANDH 64
    for (int b0 = wy0; b0 <= wy1; b0 += BANDH) {
        int b1 = b0 + BANDH - 1;
        if (b1 > wy1) b1 = wy1;
        for (int x = 0; x < 466; x++) {
            int nsp = s_spn[x];
            if (!nsp) continue;
            /* 🚨 Depth is measured as "how much water is above this pixel".
             * Measuring from the head of the run made every run restart at
             * zero, so a bright line appeared at the top of each one.
             * Measuring from the head of the column's first run instead gave
             * vertical seams: one droplet thrown up above a column makes
             * everything under it read as very deep water while the next
             * column does not.
             * Counting the thickness actually stacked above fixes both. A
             * floating droplet only contributes its own thickness and does
             * not disturb the colour below it. */
            int above = 0;
            for (int k = 0; k < nsp; k++) {
                int t0 = s_sp0[x][k], t1 = s_sp1[x][k];
                int a0 = t0 < b0 ? b0 : t0;
                int a1 = t1 > b1 ? b1 : t1;
                if (a1 >= a0) {
                    for (int y = a0; y <= a1; y++) {
                        int q = above + (y - t0);
                        s_img[(size_t)y * 466 + x] =
                            (k == 0 && q < 3) ? s_scol[x * 3 + q]
                                              : s_depth_lut[q > 200 ? 200 : q];
                    }
                }
                above += t1 - t0 + 1;
            }
        }
    }
    #undef BANDH
    if (wy1 >= wy0) {
        if (wy0 < ny0) ny0 = wy0;
        if (wy1 > ny1) ny1 = wy1;
    }
    if (ny1 < ny0) { ny0 = 0; ny1 = 0; }      /* there was no water at all */
    s_img_y0 = ny0;
    s_img_y1 = ny1;
    s_img_strip = s_img_dsc;
    s_img_strip.header.h  = (uint32_t)(ny1 - ny0 + 1);
    s_img_strip.data      = (const uint8_t *)(s_img + (size_t)ny0 * 466);
    s_img_strip.data_size = (size_t)(ny1 - ny0 + 1) * 466 * 2;
}

/* ── one frame ─────────────────────────────────────────────── */
/* 🔋 Still water that nobody is touching has nothing to compute.
 * Stepping the particles is among the most expensive things this device does,
 * and solving motionless water is simply burning battery. Once it settles it
 * rests, and any movement or any touch wakes it immediately. */
static bool water_is_still(void)
{
    float e = 0;
    for (int i = 0; i < NP; i += 4)          /* every fourth one is enough to tell */
        e += fabsf(s_vx[i]) + fabsf(s_vy[i]);
    return e < (NP / 4) * 1.2f;
}


/* ── frame-time meter ─────────────────────────────────────────
 * 🔋 Measuring power starts with knowing how much CPU a frame costs.
 * Averaged and logged as one line — the logging must not become the cost. */
static void frame_tick(const char *who, int64_t t0)
{
    static uint32_t n; static uint64_t sum; static uint32_t hi; static int64_t since;
    /* 🚨 Timing only the computation is half the story: it leaves out what
     * LVGL then paints and what is pushed to the panel. This app was the
     * clearest case — build_spans() only prepares data and the painting
     * happens later, so it reported "25 fps" while the screen plainly was
     * not. Measure the **real interval** until this is called again. That is
     * the speed a person sees. */
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
    /* 🚨 This used to log every 128 frames, which meant something as slow as
     * the water never reached the count and never printed. The slower it got,
     * the more silent it became. Every three seconds now. */
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
    water_fast(!launcher_screen_is_off());

    /* 🔋 Nothing to draw with the display off. 🚨 Do not change the period
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so from a timer callback the handler restarts and never returns.
     * Leave the period and act on every 60th call. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 60) return;
    }

    int64_t _t0 = esp_timer_get_time();
    uint32_t now = lv_tick_get();
    float dt = s_last_ms ? (now - s_last_ms) / 1000.0f : 0.033f;
    s_last_ms = now;
    if (dt > 0.10f) dt = 0.10f;

    /* ── gravity ────────────────────────────────────────────
     * 🚨 The IMU axes are rotated 90 degrees from the screen's: gx drives the
     * vertical (forward and back), gy the horizontal. Same as the marble.
     *
     * 🚨 This used to look only at *which way* it was tilted and hard-code
     * the magnitude at 1400. Shaking it up and down then did nothing at all,
     * because a shake changes the magnitude, not the direction.
     * Now all three axes are taken as they are. Shake downward and the water
     * gets heavy and presses down; flick it up and it lightens and rises.
     * Shaking straight at the screen (Z) also changes the magnitude, so that
     * is caught too. */
    float ax, ay, az = 1000.0f;
    bool have = port_imu_accel3(&ax, &ay, &az);
    if (!have) { ax = s_fake_gx; ay = s_fake_gy; az = 1000.0f; }

    /* ── tilting counts as input ────────────────────────────
     * 🚨 The launcher measures idleness with lv_display_get_inactive_time(),
     * which counts touches only. The water is steered by tilting, so the
     * screen is never touched and it went dark after thirty seconds in the
     * middle of playing with it. Same problem as the tilt games, solved the
     * same way.
     *
     * Stay awake only while the attitude is actually changing or it is being
     * shaken — put it down and it sleeps as usual. 🚨 Water sloshing under
     * its own momentum is not input, which is why this watches **the IMU**
     * and not the waves. */
    if (have) {
        static float wx, wy, wz; static bool wset;
        if (!wset) { wx = ax; wy = ay; wz = az; wset = true; }
        float d = fabsf(ax - wx) + fabsf(ay - wy) + fabsf(az - wz);
        if (d > 45.0f) {              /* a shaky hand is not input */
            wx = ax; wy = ay; wz = az;
            lv_display_trigger_activity(NULL);
        }
    }

    /* 🚨 This used to pin the attitude at app-open as "level" (s_g0x/s_g0y)
     * and only look at tilt away from that, and it clamped the angle to ±60
     * degrees. So however you turned it, the water only went "down relative
     * to how you were holding it", and a full rotation was not expressible at
     * all ("whichever way I turn it, the water should go down that way").
     *
     * Now real gravity is used as it comes. The axes were measured on the
     * board: held upright it read ax=-960 ay=-50 az=+120. The accelerometer
     * is positive on the axis pointing at the sky (flat gives az=+1000), so
     * -x is up the screen, which makes +x down. Gravity is the negation of
     * what is read:
     *     screen right = -ay      screen down = -ax
     * 🚨 This agrees with the shake code (kick_x=-jy, kick_y=-jx). Two
     * independent derivations landing on the same answer. */
    float dirx = -ay, diry = -ax;
    float dlen = sqrtf(dirx * dirx + diry * diry);
    /* 🚨 Lying flat (screen to the sky) leaves almost no gravity in the
     * screen plane. Forcing a direction out of that spins on noise — just
     * call it down. */
    if (dlen < 120.0f) { dirx = 0.0f; diry = 1.0f; }
    else               { dirx /= dlen; diry /= dlen; }

    /* Magnitude — held still it is 1 g, so 1.0. Shaking swings it either way.
     * Too large and particles jump past their neighbours and the water tears,
     * so it is clamped both ends. */
    float mag = 1.0f;
    if (have) {
        mag = sqrtf(ax * ax + ay * ay + az * az) / 1000.0f;
        if (mag < 0.25f) mag = 0.25f;
        if (mag > 2.6f)  mag = 2.6f;
    }

    /* ── catching a shake in any direction ───────────────────
     * 🚨 Magnitude alone means poking it straight at the screen (Z) presses
     * the water down but never splashes it, because that force has no
     * direction. Each axis is measured for "how suddenly did this change" and
     * the water is pushed by that:
     *   a flick along x or y  -> it surges the other way (the bowl was struck
     *                            sideways)
     *   a poke along z        -> no direction, so it scatters outward
     * This is the force that makes water hit the wall when a bowl is shaken. */
    static float px_, py_, pz_;
    static bool  pfirst = true;
    float jx = 0, jy = 0, jz = 0;
    if (have) {
        if (pfirst) { px_ = ax; py_ = ay; pz_ = az; pfirst = false; }
        jx = ax - px_; jy = ay - py_; jz = az - pz_;
        /* 🚨 With a plain threshold and a proportional response, a small
         * movement barely over the line still delivers exactly that much —
         * "the slightest movement makes the water go wild".
         * The excess is fed in squared instead: just over does almost
         * nothing, well over does the full amount. And it does not jump at
         * the threshold, since it joins at zero. */
        jx = shake_curve(jx); jy = shake_curve(jy); jz = shake_curve(jz);
        px_ = ax; py_ = ay; pz_ = az;
    }
    /* The axis mapping is the marble's: x is the screen's vertical, y the horizontal */
    /* 🚨 Shaking scattered it so widely it looked like dust. The strength is
     * halved — heavy things travel less for the same shake. */
    float kick_x = -jy * 1.1f, kick_y = -jx * 1.1f;
    float burst  = fabsf(jz) * 1.3f;      /* the straight-at-the-screen poke */
    /* 🚨 Gravity was lowered from 1400 to 1100 and then put back. That was
     * the wrong prescription — **lowering gravity does not make it heavy, it
     * makes it lunar**, floating about, which is much of why it looked like
     * dust.
     * Weight comes from three things: (1) falling properly fast when it
     * falls, (2) travelling less for a given shake (the strength above), and
     * (3) being slow to notice which way is down (the inertia below).
     * Gravity is (1), so if anything it should go up. */
    const float G = 1900.0f;
    float want_x = G * mag * dirx;
    float want_y = G * mag * diry;
    /* The direction follows a little behind (the water's inertia) but the
     * magnitude is applied at once — a shake is brief, and delaying it means
     * it is never felt. */
    s_gx += (want_x - s_gx) * 12.0f * dt;
    s_gy += (want_y - s_gy) * 12.0f * dt;
    if (have) {
        /* 🚨 0.55 was effectively instant: tilt it and the water knew which
         * way was down immediately and surged all at once. That was a large
         * part of why it felt light. Water is heavy and finds out late. 0.22. */
        s_gx = s_gx * 0.86f + want_x * 0.14f;
        s_gy = s_gy * 0.86f + want_y * 0.14f;
    }

    /* Kept so the boat and the renderer use the same "down". */
    {
        float gl = sqrtf(s_gx * s_gx + s_gy * s_gy);
        if (gl > 1.0f) { s_gux = s_gx / gl; s_guy = s_gy / gl; }
    }

    /* 🚨 PBF corrects positions rather than applying forces, so it rarely
     * blows up — but too long a step lets particles jump past their
     * neighbours and the water tears. Split into 8 ms steps.
     *
     * 🚨 The number of steps is capped. Without a cap, slower means larger dt,
     * larger dt means more steps, and more steps means slower — a death
     * spiral (at 8 fps it was six steps a frame and 106 ms in physics alone). */
    /* 🚨 The cap used to be two steps, because the physics queued behind
     * drawing on core 0. It is on core 1 now and that reason is gone. Capped,
     * a frame only computes 20 ms of water and it flows at a quarter speed —
     * splashing is brief enough to hide it, but settling and clumping take
     * time and the slowness is obvious ("the way it settles and comes back
     * together doesn't match real physics"). Compute the time that actually
     * passed. */
    int sub = (int)(dt / 0.010f) + 1;
    /* 🚨 If core 1 cannot finish within a frame, drawing waits for it and the
     * frame collapses. How long a step takes depends on the particle count
     * and the state of the water, so the number must not be hard-coded — it
     * is decided from how long the previous step actually took.
     * 🚨 60% was the first guess and it was too mean. Core 0 uses most of the
     * frame anyway (79 of 80 ms), so core 1 can use as much without either
     * waiting on the other. 90%, leaving 10% for handover and jitter. */
    if (s_phys_us && s_phys_sub > 0) {
        float per = (float)s_phys_us / (float)s_phys_sub / 1000000.0f;
        int room = (int)(dt * 0.9f / (per > 1e-6f ? per : 1e-6f));
        if (room < 1) room = 1;
        if (sub > room) sub = room;
    } else if (sub > 2) {
        sub = 2;                  /* nothing measured yet — start carefully */
    }
    float sdt = dt / sub;
    /* 🚨 Capping only the step *count* makes each step coarser as the frame
     * slows. At a 311 ms frame a step was 33 ms, and PBF recovers velocity as
     * distance over dt — so the recovered velocities ran away and the water
     * left the bowl entirely. The step length is capped as well: slow just
     * means slow, not exploded. */
    if (sdt > 0.010f) sdt = 0.010f;

    /* ── hand the physics to core 1 ───────────────────────
     * 🚨 The order matters: confirm the previous step finished, take the
     * copy, then start the next one. Nothing touches the arrays while the
     * copy is being taken. */
    phys_wait();
#ifndef BADGE_SIM
    memcpy(s_rx,  s_x,  NP * sizeof(float));
    memcpy(s_ry,  s_y,  NP * sizeof(float));
    memcpy(s_rvx, s_vx, NP * sizeof(float));
    memcpy(s_rvy, s_vy, NP * sizeof(float));
#endif
    s_pin.gx = s_gx; s_pin.gy = s_gy;
    s_pin.kx = kick_x; s_pin.ky = kick_y; s_pin.burst = burst;
    s_pin.sdt = sdt;   s_pin.sub = sub;
#ifdef BADGE_SIM
    phys_round();                 /* the simulator has one core */
#else
    s_inflight = true;
    xSemaphoreGive(s_go);
#endif


    /* ── the boat ───────────────────────────────────────────
     * The particles around it hold it up and carry it. Water washing over it
     * pushes it under.
     * 🚨 "Down" used to mean the screen's vertical axis, so turning the badge
     * left the boat lying on its side floating in the wrong direction. The
     * same arithmetic now runs in a frame where gravity is down — with
     * gravity pointing down the screen it behaves exactly as before. */
    #define BOAT_G 1900.0f               /* same gravity as the water */
/* 🚨 The hull runs 11 px above the origin (deck) and 6 px below (keel), in
 * paint_boat's coordinates. With equilibrium 9 px below the origin the deck
 * sits level with the surface and **only the mast shows**. The reference line
 * is raised so the origin floats 3 px above the surface: keel 3 px under,
 * deck 14 px clear. Equilibrium draught (BOAT_G/K) is 9, so raising the
 * reference by 12 gives bA-top = 9-12 = -3. */
#define BOAT_LIFT   12.0f
/* 🚨 Once fully submerged, buoyancy stops increasing — the force is the
 * displaced volume and the volume is used up. Without this cap the boat
 * **becomes a rocket the moment water washes over it**: top is the highest
 * particle in that band, so spray landing on the deck sends top up and depth
 * explodes. Clamped at the depth where the hull is fully under. */
#define BOAT_SUB_MAX 26.0f
    float bC = G_CROSS(s_boat_x, s_boat_y), bA = G_ALONG(s_boat_x, s_boat_y);
    float vC = s_boat_vx * s_guy - s_boat_vy * s_gux;
    float vA = s_boat_vx * s_gux + s_boat_vy * s_guy;

    float svc = 0; int n = 0;
    float top = 1e9f;
    /* 🚨 Water tilted under the boat should carry it down the slope. It did
     * not, because buoyancy was applied **only along gravity** — however
     * steeply the surface tilted, no sideways force appeared at all and the
     * boat sat on the slope. Real buoyancy is perpendicular to the surface,
     * so a surface tilted by theta leaves g*sin(theta) along the slope once
     * combined with gravity.
     * The surface height is measured left and right separately to get that. */
    float tl = 1e9f, tr = 1e9f;
    for (int i = 0; i < NP; i++) {
        float c = G_CROSS(s_rx[i], s_ry[i]) - bC;
        if (fabsf(c) > 34) continue;
        svc += s_rvx[i] * s_guy - s_rvy[i] * s_gux;
        n++;
        float a = G_ALONG(s_rx[i], s_ry[i]);
        if (a < top) top = a;            /* highest water, against gravity */
        if (c < 0) { if (a < tl) tl = a; }
        else       { if (a < tr) tr = a; }
    }
    /* Positive when the right side is lower (a is larger) — it slides that way. */
    float slope = 0.0f;
    if (tl < 1e8f && tr < 1e8f) {
        slope = (tr - tl) / 34.0f;
        /* 🚨 A single droplet of spray on one side gives an absurd slope.
         * sin(theta) cannot exceed 1, so it is clamped around there. */
        if (slope >  0.6f) slope =  0.6f;
        if (slope < -0.6f) slope = -0.6f;
    }
    /* 🚨 This was one spring toward the surface, and that is what "it bounces
     * like a ball" was — **the surface pulled the boat even out of the
     * water**. Dropped from height it was dragged down in proportion to the
     * distance and hit harder than it should, then thrown back out with the
     * same force.
     * In and out of the water are now separate cases:
     *     out — it simply falls; the surface exerts nothing
     *     in  — it floats by how deep it is, and deeper water grips harder
     * That gives a splash going in and a heavy rise coming out. */
    /* 🚨 The boat alone is sub-stepped. The app runs at about 12 fps, and
     * buoyancy stiff enough to float it shallow brings the bobbing period
     * down to 0.43 s — five frames per bob. At that resolution the integrator
     * cannot see an oscillation as an oscillation and flattens it, so
     * **the bobbing disappears numerically**. The boat is a handful of
     * scalars, so sub-stepping it is nearly free. The water state (top, svc,
     * n) is held fixed for the frame while the boat takes several steps. */
    int bsub = (int)(dt / 0.02f) + 1;
    if (bsub > 8) bsub = 8;
    float bdt = dt / (float)bsub;

    for (int bs_i = 0; bs_i < bsub; bs_i++) {
        /* Add back the raised reference — zero here means "floating where it should". */
        float depth = bA - top + BOAT_LIFT;
        if (depth > BOAT_SUB_MAX) depth = BOAT_SUB_MAX;   /* fully under */
        if (n && depth > 0.0f) {
            /* 🚨 Buoyancy proportional to submerged depth (Archimedes). The
             * stiffness K is set by the draught: weight balances at the
             * settled depth, so draught = BOAT_G/K. 210 aims at 9 px (76 gave
             * 25 px, which looked sunk). */
            const float K = 210.0f;
            /* 🚨 Damping decides how many times it bobs before settling. Kept
             * light so it keeps hopping just clear of the water and dropping
             * back (a damping ratio of 0.14 cuts the height to a third per
             * bounce). */
            float f = depth / BOAT_SUB_MAX;
            float C = 4.0f + 6.0f * f;
            /* 🚨 Resistance differs going in and coming out. Equal values
             * make the first dive too shallow ("it should go deeper when it
             * first drops"). A hull entering water drags air down with it and
             * meets less resistance; coming up it pushes a whole column of
             * water ahead of it. */
            if (vA > 0.0f) C *= 0.55f;   /* on the way down */
            /* 🚨 `vA += (k*error - c*vA)*dt` is explicit and diverges once
             * k*dt exceeds 2. Solved implicitly instead — the denominator is
             * always greater than one, so it cannot blow up. */
            float denom = 1.0f + C * bdt + K * bdt * bdt;
            vA = (vA + (BOAT_G - K * depth) * bdt) / denom;
            /* 🚨 The rising speed used to be clamped at -260, which **removed
             * the force that gets it out of the water** — bouncing clear
             * could not happen at all. Not clamped. */
            /* How much the current carries it. At 3.0 it is swept along like a leaf. */
            vC += ((svc / n) - vC) * (1.3f * bdt / (1.0f + 1.3f * bdt));
            /* It runs down the slope. g*sin(theta) is the sideways
             * acceleration directly — there is no separate coefficient,
             * because the physics already decided the number. */
            vC += BOAT_G * slope * bdt;
        } else {
            vA += BOAT_G * bdt;          /* airborne — the surface exerts nothing */
            /* Airborne it is barely carried by the current. Setting it to
             * zero means sideways speed never decays and it drifts along the
             * wall of the bowl. */
            if (n) vC += ((svc / n) - vC) * (0.35f * bdt / (1.0f + 0.35f * bdt));
        }
        bA += vA * bdt;
        bC += vC * bdt;
    }

    /* Back from the gravity frame to screen coordinates — the inverse of
     * G_ALONG/G_CROSS. (gux,guy) is a unit vector, so this is a rotation and
     * the expression is exact. */
    s_boat_x = CX + bA * s_gux + bC * s_guy;
    s_boat_y = CY + bA * s_guy - bC * s_gux;
    s_boat_vx = vC * s_guy + vA * s_gux;
    s_boat_vy = -vC * s_gux + vA * s_guy;
    /* Keep it inside the bowl */
    float bdx = s_boat_x - CX, bdy = s_boat_y - CY;
    float bd = sqrtf(bdx * bdx + bdy * bdy);
    if (bd > R - 40) {
        float k = (R - 40) / bd;
        s_boat_x = CX + bdx * k;
        s_boat_y = CY + bdy * k;
        s_boat_vx *= -0.3f;
        s_boat_vy *= -0.3f;
    }

    /* 🔋 Rest when it is calm. Solving every third frame looks identical and
     * lets the CPU idle in between; a shake or a tilt brings it straight back.
     * 🚨 But when the frame rate is already low, skipping two out of three
     * becomes one frame every half second and reads as "it froze". Only skip
     * when there is frame time to spare — when it is slow there is nothing to
     * save. */
    if (s_still_flag && dt < 0.070f) {
        static uint8_t idle;
        if (++idle % 3) return;
    }

    /* 🚨 Splitting where the time goes is the only way to know what to fix:
     * solving particles, or building the picture. One combined number sends
     * you off sharpening the wrong thing. */
    int64_t _tp = esp_timer_get_time();
    build_spans();
    int64_t _ts = esp_timer_get_time();
    paint_img();
#ifdef BADGE_SIM
    paint_verify();
#endif
    /* 🚨 Invalidating the whole screen makes LVGL background-fill, copy and
     * DMA all 466 rows. The water is usually in the lower half, so that is
     * three passes of double waste (transfer was 78 ms, against a floor of
     * 21.7 ms for one full frame).
     * Only the changed area is invalidated — the union of last frame's area
     * and this one's, or the places water has left do not get erased. The
     * boat leaves the water, so it is given generous margins. */
    {
        /* What will be drawn this frame = the water band plus the boat's band.
         * 🚨 The boat is taller when it turns: with the mast its local y runs
         * -52..+6, and rotated 90 degrees that becomes its horizontal extent
         * (-30..+33), so ±60 is allowed. */
        int y0 = s_img_y0, y1 = s_img_y1;
        int by0 = (int)s_boat_y - 60, by1 = (int)s_boat_y + 60;
        if (by0 < y0) y0 = by0;
        if (by1 > y1) y1 = by1;
        if (y0 < 0) y0 = 0;
        if (y1 > 465) y1 = 465;

        /* 🚨 The whole area the previous frame occupied has to be unioned in.
         * Unioning only the water band left the boat's wake and departed
         * water on screen for a while ("some things take time to disappear"). */
        static int py0 = 0, py1 = 465;
        int i0 = y0 < py0 ? y0 : py0;
        int i1 = y1 > py1 ? y1 : py1;
        py0 = y0; py1 = y1;

        lv_area_t inv = { 0, i0, 465, i1 };
        lv_obj_invalidate_area(s_field, &inv);
    }
    int64_t _te = esp_timer_get_time();
    static int64_t phys, span, pnt; static uint32_t cnt; static int64_t since2;
    phys += _tp - _t0; span += _ts - _tp; pnt += _te - _ts; cnt++;
    if (!since2) since2 = _te;
    if (_te - since2 >= 3000000) {
        ESP_LOGI("water", "%u splits — handover %llu, runs %llu, paint %llu us"
                 " | core1 %u us (%d steps)",
                 (unsigned)cnt, (unsigned long long)(phys / cnt),
                 (unsigned long long)(span / cnt), (unsigned long long)(pnt / cnt),
                 (unsigned)s_phys_us, s_phys_sub);
        phys = span = pnt = 0; cnt = 0; since2 = _te;
    }
    frame_tick("water", _t0);
}

static void tap_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        /* No IMU in the simulator. Pretend it is tilted toward the finger. */
        lv_indev_t *in = lv_indev_active();
        lv_point_t p = { CX, CY };
        if (in) lv_indev_get_point(in, &p);
        water_sim_tilt(p.x, p.y);
        return;
    }
    if (code == LV_EVENT_RELEASED) { water_sim_tilt(CX, CY); return; }
}

/* Allocated from PSRAM in one go. If that fails, the water is skipped. */
static bool alloc_all(void)
{
    #define GET(v, n) do { v = heap_caps_malloc((n), MALLOC_CAP_SPIRAM); \
                           if (!(v)) return false; } while (0)
    /* 🚨 Moving the particle arrays into internal RAM was tried and reverted.
     * The theory was that a 26 ms step was waiting on PSRAM, but moving them
     * left it at 27.6 ms — it is not waiting, the arithmetic really is that
     * expensive. Meanwhile the internal heap dropped from 89 to 72 KB and the
     * largest block from 50 to 32 KB, which only raises the risk of drawing
     * failing to get a large buffer. Measure again before retrying. */
    GET(s_x, NP * 4);      GET(s_y, NP * 4);
    GET(s_px, NP * 4);     GET(s_py, NP * 4);
    GET(s_vx, NP * 4);     GET(s_vy, NP * 4);
    GET(s_next, NP * 2);
    GET(s_dens, (size_t)NROW * NCOL);
    GET(s_chur, (size_t)NROW * NCOL);
    GET(s_sp0, (size_t)466 * SPANS * 2);
    GET(s_sp1, (size_t)466 * SPANS * 2);
    GET(s_spn, 466);       GET(s_sfrac, 466);
    GET(s_slope, 466);     GET(s_foam, 466);
    GET(s_pv0, 466 * 2);   GET(s_pv1, 466 * 2);
    /* 🚨 These say where the water was last frame, and they are read before
     * they are first written. Left as malloc gave them, the clearing loops run
     * with garbage for a y range and write outside the image — which lands as
     * a cache error panic the moment the app opens, not as a wrong pixel.
     * p0 > p1 means the column was empty. */
    for (int i = 0; i < 466; i++) { s_pv0[i] = 1; s_pv1[i] = 0; }
    GET(s_cur, 466 * (int)sizeof(int));
    GET(s_scol, 466 * 3 * 2);
#ifdef BADGE_SIM
    /* One core, so there is nothing to copy — point at the same arrays */
    s_rx = s_x; s_ry = s_y; s_rvx = s_vx; s_rvy = s_vy;
#else
    GET(s_rx, NP * 4);     GET(s_ry, NP * 4);
    GET(s_rvx, NP * 4);    GET(s_rvy, NP * 4);
    memcpy(s_rx,  s_x,  NP * sizeof(float));
    memcpy(s_ry,  s_y,  NP * sizeof(float));
    memcpy(s_rvx, s_vx, NP * sizeof(float));
    memcpy(s_rvy, s_vy, NP * sizeof(float));
#endif
    /* 🚨 calloc, not malloc. With the memset gone (see paint_img's clearing
     * comment) anywhere nobody paints goes to the screen exactly as allocated. */
    s_img = heap_caps_calloc((size_t)466 * 466, 2, MALLOC_CAP_SPIRAM);
    if (!s_img) return false;
    #undef GET
    depth_lut_build();
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = 466;
    s_img_dsc.header.h      = 466;
    s_img_dsc.header.stride = 466 * 2;
    s_img_dsc.data          = (const uint8_t *)s_img;
    s_img_dsc.data_size     = (size_t)466 * 466 * 2;
    return true;
}

static void free_all(void)
{
    #define PUT(v) do { if (v) { heap_caps_free(v); v = NULL; } } while (0)
    PUT(s_x); PUT(s_y); PUT(s_px); PUT(s_py); PUT(s_vx); PUT(s_vy);
    PUT(s_next); PUT(s_dens); PUT(s_chur);
    PUT(s_sp0); PUT(s_sp1); PUT(s_spn); PUT(s_sfrac); PUT(s_slope); PUT(s_foam);
    PUT(s_cur); PUT(s_scol); PUT(s_pv0); PUT(s_pv1);
#ifdef BADGE_SIM
    s_rx = s_ry = s_rvx = s_rvy = NULL;      /* these were pointing at someone else's memory */
#else
    PUT(s_rx); PUT(s_ry); PUT(s_rvx); PUT(s_rvy);
#endif
    PUT(s_img);
    #undef PUT
}

lv_timer_t *water_start(lv_obj_t *root)
{
    s_root = root;
    if (!alloc_all()) {
        free_all();
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, "no memory");
        lv_obj_center(l);
        return NULL;
    }

    /* Laid out on a grid in the lower half. Every other row shifts half a cell or it stripes. */
    int n = 0;
    for (int row = 0; row < 44 && n < NP; row++) {
        float y = CY - 14 + row * 10.2f;
        for (int col = -23; col <= 23 && n < NP; col++) {
            float x = CX + col * 10.2f + (row & 1 ? 5.1f : 0);
            float dx = x - CX, dy = y - CY;
            if (dx * dx + dy * dy > (float)(R - 8) * (R - 8)) continue;
            s_x[n] = x; s_y[n] = y;
            s_vx[n] = s_vy[n] = 0;
            n++;
        }
    }
    for (; n < NP; n++) { s_x[n] = CX; s_y[n] = CY + 120; s_vx[n] = s_vy[n] = 0; }

    /* 🚨 The reference density is measured from this very arrangement. Hard
     * coding it means the water swells or collapses the moment the particle
     * count or spacing changes. Edge particles read low for want of
     * neighbours, so only the interior is counted. */
    grid_build_from_current();
    float sum = 0; int cnt = 0;
    for (int i = 0; i < NP; i++) {
        float dx = s_x[i] - CX, dy = s_y[i] - CY;
        if (dx * dx + dy * dy > (float)(R - 40) * (R - 40)) continue;
        float rho = 0;
        for (int j = 0; j < NP; j++) {
            if (j == i) continue;
            float ex = s_x[j] - s_x[i], ey = s_y[j] - s_y[i];
            float r2 = ex * ex + ey * ey;
            if (r2 >= HR2 || r2 < 1e-4f) continue;
            float q = 1.0f - sqrtf(r2) / HR;
            rho += q * q;
        }
        sum += rho; cnt++;
    }
    s_rho0 = cnt ? sum / cnt : 3.0f;

    s_field = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_field);
    lv_obj_set_size(s_field, 466, 466);
    lv_obj_center(s_field);
    lv_obj_clear_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_field, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_field, cover_cb, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_RELEASED, NULL);

    s_hint = lv_label_create(s_root);
    lv_label_set_text(s_hint, "tilt");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A6478), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 200);

    s_gx = 0; s_gy = 1400.0f;
    s_boat_x = CX; s_boat_y = CY - 20;
    s_boat_vx = s_boat_vy = 0;
    s_last_ms = 0;
#ifndef BADGE_SIM
    /* 🚨 Pinned to core 1: main and BLE are both on core 0, so it is free.
     * Priority below LVGL — late physics is better than a frozen display. */
    if (!s_phys) {
        s_go   = xSemaphoreCreateBinary();
        s_done = xSemaphoreCreateBinary();
        s_inflight = false;
        if (!s_go || !s_done ||
            xTaskCreatePinnedToCore(phys_task, "water", 4096, NULL, 3, &s_phys, 1) != pdPASS) {
            ESP_LOGI("water", "could not start the core-1 task — running on core 0");
            s_phys = NULL;
        }
    }
#endif
    s_loop = lv_timer_create(step, 33, NULL);
    return s_loop;
}

void water_stop(void)
{
    water_fast(false);           /* 🚨 left held, the badge sits at 240 MHz forever */
    s_loop = NULL;               /* the caller deletes the timer */
    /* 🚨 Delete the image before freeing the memory. The other way round
     * leaves a live s_field trying to draw particle arrays that are already
     * gone — pressing home drew one more frame in between and died. The same
     * thing was found and fixed in the globe, and then repeated here. */
    if (s_field) { lv_obj_delete(s_field); s_field = NULL; }
    s_root = s_hint = NULL;
    /* 🚨 Core 1 may be in the middle of the arrays. Wait for the step to
     * finish and delete the task before freeing. The other order writes into
     * freed memory — the same accident as the drawing one above. */
    phys_wait();
#ifndef BADGE_SIM
    if (s_phys) { vTaskDelete(s_phys); s_phys = NULL; }
    if (s_go)   { vSemaphoreDelete(s_go);   s_go = NULL; }
    if (s_done) { vSemaphoreDelete(s_done); s_done = NULL; }
#endif
    free_all();                  /* always give the PSRAM back */
}

/* Simulator only — tilt toward the finger, standing in for the IMU */
void water_sim_tilt(int x, int y)
{
    s_fake_gx = -((float)y - CY) * 4.0f;
    s_fake_gy = -((float)x - CX) * 4.0f;
}

/* For tests — gravity angle, boat position, and how many particles escaped the bowl */
void water_debug(float *deg, float *boat, float *escaped)
{
    if (deg)  *deg  = atan2f(s_gx, s_gy) / DEG2RAD;
    if (boat) *boat = s_boat_x - CX;
    if (escaped) {
        int n = 0;
        for (int i = 0; i < NP; i++) {
            float dx = s_x[i] - CX, dy = s_y[i] - CY;
            if (dx * dx + dy * dy > (float)R * R + 1) n++;
        }
        *escaped = (float)n;
    }
}
