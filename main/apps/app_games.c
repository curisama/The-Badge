/* The games. All of them move a handful of shapes, so none of them cost much
 * CPU or memory.
 *
 *   brick breaker — a ring paddle and bricks laid round the circle; a shape
 *                   a rectangular screen cannot make
 *   pinball       — the round screen is the table
 *   marble maze   — tilt to roll it, which puts the idle IMU to work
 *   bubble wrap   — pop them, they come back */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include "water.h"
#include "orb.h"
#include <math.h>
#include <stdlib.h>

#define CX      233
#define CY      233
#define DEG2RAD 0.0174533f


#ifdef BADGE_SIM
#  include <stdio.h>
#  define ESP_LOGI(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#  include "esp_log.h"
#endif

/* clear_board walks these arrays, so the counts have to be settled here. */
#define BRK_ROWS   4            /* a level uses three rows or four */
/* 🚨 Without spare balls, one miss rebuilds the level from scratch. Getting
 * most of the way through five levels and losing all of it to a single miss
 * is where people put the badge down — reported as "cleared it, died, started
 * completely over, furious".
 * Three balls: two mistakes forgiven, the third ends it. */
#define BRK_LIVES  3
#define BRK_MAX    9            /* most bricks in one row */
#define BRK_N      (BRK_ROWS * BRK_MAX)

/* Reference attitude for the tilt games. The tilt at the moment you tap to
 * start becomes level, so whether the badge is on your wrist or flat on a
 * desk, "this is horizontal" means the same thing and it rolls the same way. */
static float s_g0x, s_g0y;
static bool  s_g0_set;

static lv_obj_t   *s_root;      /* the board everything is drawn on */
/* Things on the board. clear_board has to drop them all at once, so they live together. */
static lv_obj_t   *s_paddle, *s_ball;
/* Tilt mode — steer by tilting the badge. Same button toggles it. */
static bool      s_tiltmode;
static lv_obj_t *s_tilt_btn, *s_tilt_lbl;
static float     s_tilt0;        /* the attitude at switch-on becomes centre */
static bool      s_tilt0_set;
static lv_obj_t   *s_brick[BRK_N];
static bool        s_alive[BRK_N];
static uint8_t     s_hp[BRK_N];      /* hits left; 2 means it takes another one */
/* The obstacle: two beads orbiting the middle.
 * 🚨 Do not build this out of an arc. Changing an arc's angle makes LVGL
 * invalidate the object's whole bounding box — at radius 124 that is
 * 262x262 = 68,000 pixels pushed every step, which halves the frame rate on
 * its own. Moving a small bead invalidates the old spot and the new one, and
 * nothing else. Same cost as the ball. */
#define OBS_N     2
#define OBS_R     124.0f        /* orbit radius */
#define OBS_D     26            /* diameter, px */
static lv_obj_t *s_obs[OBS_N];
static float     s_obs_ang, s_obs_dps;

#define PB_WALL     206.0f   /* how far out the ball's centre may go */
#define PB_BR       6.5f     /* ball radius */
#define PB_GRAV     0.085f   /* downward pull per 20 ms step — the table's slope */
/* Tilting a full 1 g (=1000) pushes slightly harder than the built-in slope,
 * so holding the badge upright adds the two together and roughly doubles the
 * speed. Steeper when you stand it up is the right behaviour. */
#define PB_TILT     0.00011f
#define PB_DAMP     0.996f
#define PB_WALL_E   0.84f    /* the wall takes a little off */
#define PB_MAXV     14.0f
#define PB_BUMP_N   5
#define PB_BUMP_R   21.0f
/* Drop targets: knocked flat when hit, and all of them stand back up together.
 * 🚨 With only bumpers, every direction scores the same and there is nothing
 * to aim at. The board only gets used widely once there is a "what do I go
 * for next". */
#define PB_TGT_N    4
#define PB_TGT_R    9.0f
#define PB_BUMP_E   1.30f    /* bumpers kick the ball away */
#define PB_FLIP_L   104.0f
#define PB_FLIP_W   14.0f
#define PB_FLIP_STEP 15.0f   /* degrees swept per step */
#define PB_DRAIN_X  44.0f    /* fall through a gap this wide and it is lost */
/* 🚨 The flipper pivots have to sit **on the wall**. They started at CX±76,
 * but the wall at that height is at CX±141, which left a 65 px lane outside
 * them — and the ball simply ran down it, ignoring the flippers entirely.
 * Putting the pivots on the circle removes the lane.
 * sqrt(206^2 - 150^2) = 141.2, which is how wide the circle is there. */
#define PB_RING_W   6
/* Inner face lands where the ball touches = (PB_WALL + PB_BR + width) * 2 */
#define PB_RING_D   ((int)((PB_WALL + PB_BR + PB_RING_W) * 2.0f))
/* 🚨 Each 20 ms step is done in three. A flipper turns 15 degrees per step
 * and its tip is 104 px from the pivot, so it sweeps 27 px in one — wider
 * than the ball (13 across). Testing only the end angle lets a raised
 * flipper pass straight through the ball and nothing happens at all. That is
 * what "the hit detection is off" was. In thirds it moves 9 px and connects. */
#define PB_SUB      3
#define PB_FLIP_PX  141.0f
#define PB_FLIP_PY  150.0f
#define PB_BALLS    3

typedef struct {
    float px, py;            /* pivot */
    float rest, up;          /* resting and raised angle (degrees, +y is down) */
    float ang;
    bool  on;
} pb_flip_t;

static pb_flip_t s_flip[2];
static lv_obj_t *s_flip_obj[2];
static lv_point_precise_t s_flip_pt[2][2];
static lv_obj_t *s_pb_ball, *s_pb_bump[PB_BUMP_N];
static float s_pb_x, s_pb_y, s_pb_vx, s_pb_vy;
static int   s_pb_pts, s_pb_left;
static uint32_t s_pb_last, s_pb_acc;
static float s_pb_bx[PB_BUMP_N], s_pb_by[PB_BUMP_N];
static float s_pb_tx[PB_TGT_N], s_pb_ty[PB_TGT_N];
static bool  s_pb_tup[PB_TGT_N];          /* still standing? */
static lv_obj_t *s_pb_tgt[PB_TGT_N];
static int         s_brick_n, s_left_cnt;
static lv_timer_t *s_loop;
static lv_obj_t   *s_score;

static void show_menu(void);

/* Starting the instant the screen appears means dying before you are ready. Wait for a tap. */
static lv_timer_cb_t s_pending_cb;
static uint32_t      s_pending_ms;
static lv_obj_t     *s_ready_lbl;

static void arm_start(lv_timer_cb_t cb, uint32_t ms)
{
    s_pending_cb = cb;
    s_pending_ms = ms;
    s_ready_lbl = lv_label_create(s_root);
    lv_label_set_text(s_ready_lbl, "tap to start");
    lv_obj_set_style_text_font(s_ready_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ready_lbl, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(s_ready_lbl, LV_ALIGN_CENTER, 0, 128);
}

/* On the first tap, start the game and do not pass that touch on to it */
static bool consume_start_tap(void)
{
    if (!s_pending_cb) return false;
    ESP_LOGI("game", "start tap — timer armed (tilt mode=%d)", (int)s_tiltmode);
    if (s_ready_lbl) { lv_obj_delete(s_ready_lbl); s_ready_lbl = NULL; }
    /* Remember this attitude as level.
     * This is only reached from a touch, so the input is already counted and
     * there is nothing to wake. */
    if (!port_imu_accel(&s_g0x, &s_g0y)) { s_g0x = s_g0y = 0; }
    s_g0_set = true;
    s_loop = lv_timer_create(s_pending_cb, s_pending_ms, NULL);
    s_pending_cb = NULL;
    return true;
}

/* ── shared ──────────────────────────────────────────────────── */

static void stop_loop(void)
{
    if (s_loop) { lv_timer_delete(s_loop); s_loop = NULL; }
}

#define POP_MAX  32
#define POP_D    78            /* bubble diameter — bigger than a fingertip or it is no fun */
#define POP_R    178           /* bubbles whose centre is inside this get popped */

static lv_obj_t *s_pop[POP_MAX];
static uint8_t   s_popped[POP_MAX];
static int       s_pop_n, s_pop_left;
static uint32_t  s_pop_seed = 2463534242u;
static lv_obj_t *s_pop_lbl;
static int       s_refill_in;  /* if >= 0, refill after this many ticks */

void pop_start(void);
static void do_orb(void);
void brk_start(void);
static void brk_rebuild(void);
static int  s_brk_life = BRK_LIVES;
void pb_start(void);
static void pb_rebuild(void);
void maze_start(void);
static void brk_step(lv_timer_t *t);

static void clear_board(void)
{
    stop_loop();
    s_pending_cb = NULL;
    s_ready_lbl = NULL;
    s_g0_set = false;
    water_stop();
    orb_stop();
    s_pop_n = s_pop_left = 0;
    s_pop_lbl = NULL;
    for (int i = 0; i < POP_MAX; i++) s_pop[i] = NULL;
    lv_obj_clean(s_root);
    /* Drop every pointer into what was just deleted. Leave one and the next
     * thing that touches it walks into freed memory before the next board is up. */
    s_score   = NULL;
    s_paddle  = NULL;
    s_tilt_btn = NULL;
    s_tilt_lbl = NULL;
    s_tiltmode = true;   /* tilt is the default */
    s_ball    = NULL;
    s_brick_n = 0;
    s_left_cnt = 0;
    for (int i = 0; i < BRK_N; i++) { s_brick[i] = NULL; s_alive[i] = false; s_hp[i] = 0; }
    for (int i = 0; i < OBS_N; i++) s_obs[i] = NULL;
    s_obs_dps = 0.0f;
    s_pb_ball = NULL;
    for (int i = 0; i < 2; i++) s_flip_obj[i] = NULL;
    for (int i = 0; i < PB_BUMP_N; i++) s_pb_bump[i] = NULL;
}

/* ── rebuild screens outside the event ────────────────────────
 * The menu and back buttons are children of s_root. Calling
 * lv_obj_clean(s_root) from inside one of their events frees the very button
 * being handled, and LVGL returns to an object that no longer exists — which
 * is what crashed after a game or two. So the actual switch goes out through
 * a one-shot 1 ms timer, outside the event.
 *
 * 🚨 port_tone_enable(true) used to be called with nothing ever calling
 * false. From the first brick onward the codec stayed open and a square wave
 * kept going out — speaker, I2S and amplifier driven for the whole game, when
 * the intent was a single tick. A one-shot timer stops it. */
static void tone_off_cb(lv_timer_t *t)
{
    (void)t;
    port_tone_enable(false);
}

static void blip(uint32_t hz, uint32_t ms)
{
    port_tone_freq(hz);
    port_tone_enable(true);
    lv_timer_t *t = lv_timer_create(tone_off_cb, ms, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void (*s_defer_fn)(void);

static void defer_cb(lv_timer_t *t)
{
    (void)t;
    void (*fn)(void) = s_defer_fn;
    s_defer_fn = NULL;
    if (fn) fn();
}

static void defer(void (*fn)(void))
{
    if (s_defer_fn) return;          /* stop a double tap queueing two */
    s_defer_fn = fn;
    lv_timer_t *t = lv_timer_create(defer_cb, 1, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void do_back(void)
{
    clear_board();
    launcher_handle_show(true);      /* the menu gets the handle back */
    show_menu();
}

static void orb_menu(void);

/* 🚨 Back out of the planets and you ended up in the games menu. Three apps
 * share this file, but back should follow **the door you came in by**, not
 * the file. So the destination is handed to the button. */
static void do_orb_back(void)
{
    clear_board();
    launcher_handle_show(true);
    orb_menu();
}

static void back_cb(lv_event_t *e)
{
    void (*dest)(void) = (void (*)(void))lv_event_get_user_data(e);
    defer(dest ? dest : do_back);
}

/* 🚨 Each board picks its own spot. The default is outside at the top
 * (-74,-196), but on the boards that draw a wall (bricks, pinball) that lands
 * on the wall itself — reported as "the wall, the bricks and the button".
 * The wall has collision on it and cannot move, so the button comes inward.
 * A 64x34 button fits inside radius R when
 *     (|dx|+32)^2 + (|dy|+17)^2 <= R^2
 * and the brick wall is the tightest at 206, so (∓40, -174) is what both use. */
#define GBTN_IN_DX   40
#define GBTN_IN_DY  (-174)

static void add_back_xy(void (*dest)(void), int dx, int dy)
{
    lv_obj_t *b = lv_button_create(s_root);
    lv_obj_set_size(b, 64, 34);
    lv_obj_set_style_radius(b, 17, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, (void *)dest);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_center(l);
}

static void add_back_to(void (*dest)(void)) { add_back_xy(dest, -74, -196); }
static void add_back(void) { add_back_to(do_back); }

/* ── tilt mode ───────────────────────────────────────────────
 * Instead of dragging the paddle, tilt the badge and let it roll. On a strap
 * around your wrist this is much better — your finger is not on the screen.
 * The same button switches back to touch. */
static void tilt_paint(void)
{
    if (!s_tilt_btn) return;
    /* Tilt is the default, so the button is the way back to dragging. It
     * shows the state it turns on (touch), not the one that is active. */
    lv_obj_set_style_bg_color(s_tilt_btn,
        lv_color_hex(s_tiltmode ? 0x24242A : 0x2E6E5A), 0);
    if (s_tilt_lbl) {
        lv_label_set_text(s_tilt_lbl, s_tiltmode ? LV_SYMBOL_REFRESH : LV_SYMBOL_LOOP);
        lv_obj_set_style_text_color(s_tilt_lbl,
            lv_color_hex(s_tiltmode ? 0x8A8A90 : 0xFFFFFF), 0);
    }
}

static void tilt_cb(lv_event_t *e)
{
    (void)e;
    s_tiltmode = !s_tiltmode;
    s_tilt0_set = false;          /* take a fresh reference attitude */
    tilt_paint();
}

static void add_tilt_btn_xy(int dx, int dy)
{
    /* 🚨 Default to tilt. This is worn on a strap, and dragging the paddle
     * means covering the screen with your own hand. */
    s_tiltmode = true;
    s_tilt0_set = false;
    s_tilt_btn = lv_button_create(s_root);
    lv_obj_set_size(s_tilt_btn, 64, 34);
    lv_obj_set_style_radius(s_tilt_btn, 17, 0);
    lv_obj_set_style_shadow_width(s_tilt_btn, 0, 0);
    lv_obj_align(s_tilt_btn, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(s_tilt_btn, tilt_cb, LV_EVENT_CLICKED, NULL);
    s_tilt_lbl = lv_label_create(s_tilt_btn);
    lv_obj_center(s_tilt_lbl);
    tilt_paint();
}

static lv_obj_t *dot(lv_obj_t *p, int d, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void put(lv_obj_t *o, float x, float y)
{
    lv_obj_set_pos(o, (int)(x - lv_obj_get_width(o) / 2),
                      (int)(y - lv_obj_get_height(o) / 2));
}

static lv_obj_t *make_score(void)
{
    lv_obj_t *l = lv_label_create(s_root);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 168);
    return l;
}

/* ── brick breaker ───────────────────────────────────────────
 * Bricks sit in a block at the top; the paddle runs along the bottom arc
 * only. It turns like a dial and cannot leave the lower half. */

/* The bricks are rectangles in rows. Each row is as wide as the circle allows,
 * but the bricks themselves are straight, so the collision test is a box. */
#define BRK_H      26
#define BRK_TOP    80           /* first row's y — below the buttons (y 42..76) */
#define BRK_GAP    4
#define PADDLE_R   198.0f
#define PAD_HALF   26.0f
#define PAD_MIN    128.0f
#define PAD_MAX    232.0f
#define BALL_R     8.0f
/* Put the **inner face** of the wall and paddle where the ball touches
 * (PADDLE_R + BALL_R = 206). For lv_arc that face is size/2 - line width. */
#define BRK_WALL_W 4
#define BRK_PAD_W  16
#define BRK_WALL_D ((int)((PADDLE_R + BALL_R + BRK_WALL_W) * 2.0f))
/* How far out bricks may go: 4 px in from the wall's inner face (206).
 * 🚨 Flush against it looks like contact, and it lets the ball touch a brick
 * and the wall **in the same step** — two reflections that cancel and send it
 * somewhere impossible. */
#define BRK_FIT    (PADDLE_R + BALL_R - 4.0f)
#define BRK_PAD_D  ((int)((PADDLE_R + BALL_R + BRK_PAD_W) * 2.0f))
/* Pixels per step (20 ms period = 50 fps). 5.0 is about 250 px/s, roughly 40%
 * quicker than the 4.5-per-25 ms it used to be. */
static uint32_t s_brk_last, s_brk_acc;   /* banked time, spent as whole steps */
#define BALL_RAMP  1.015f       /* a little faster on every paddle hit */

/* ── five levels ──────────────────────────────────────────────
 * 🚨 **Speed alone is not the difficulty.** The physics takes a fast ball
 * fine: bricks are 26 px thick, so tunnelling needs 42 px in one step; the
 * paddle is tested by radius, so passing through it is not possible at all;
 * and steps are timed, so a late frame does not skip one either.
 * What cannot take it is **a wrist**. Top to paddle is 1.6 s at 5.0 and 0.66 s
 * at 12, and this is steered by tilt, not touch — you have to physically turn
 * your wrist, and the paddle only travels 104 degrees. At 0.66 s it stops
 * being hard and becomes luck. So speed tops out at 9.0 and the other four
 * dials carry the rest.
 *
 * 🚨 Set **both** the starting speed and the cap. The ball gains 1.5% on each
 * paddle hit until it reaches the cap (BALL_RAMP). Raise only the cap and
 * every level opens identically; raise only the start and it pins to the cap
 * after a few bounces and the levels feel the same anyway. */
#define BRK_LEVELS 5
typedef struct {
    float   spd0, spdmax;   /* starting speed and cap, px per step */
    float   pad_half;       /* paddle half-angle; smaller is a shorter paddle */
    uint8_t rows;           /* rows of bricks (3 or 4) */
    uint8_t pattern;        /* 0 = full, 1 = checker, 2 = gap in the middle */
    uint8_t tough_rows;     /* this many rows from the top take two hits */
    float   obs_dps;        /* obstacle speed, degrees per step; 0 = none */
} brk_level_t;

/* 🚨 A later level must not **finish sooner** than an earlier one. Thinning
 * the layout to raise difficulty also removes bricks and shortens the round —
 * the first cut had L4 at 8 bricks against L1's 17, which is backwards. Levels
 * that thin out get an extra row. */
static const brk_level_t BRK_LV[BRK_LEVELS] = {
    /* start  cap   half  rows pattern tough obstacle */
    { 5.0f, 6.5f, 26.0f, 3, 0, 0, 0.0f },
    { 5.4f, 7.0f, 24.0f, 4, 0, 1, 0.0f },
    { 5.8f, 7.5f, 22.0f, 4, 1, 2, 1.2f },
    { 6.4f, 8.2f, 20.0f, 4, 2, 2, 1.8f },
    { 7.0f, 9.0f, 18.0f, 4, 0, 2, 2.6f },
};
static int   s_level;                    /* 0..4 */
static float s_pad_half = 26.0f;         /* this level's paddle half-angle */
static float s_spd0, s_spdmax;




static lv_area_t s_brect[BRK_N];
static float     s_bx, s_by, s_vx, s_vy, s_pad_ang;
static float     s_spd;   /* target speed now; creeps up on each paddle hit */

static void paddle_draw(void)
{
    /* LVGL arcs put 0 degrees at 3 o'clock; ours is from 12, hence the -90. */
    float g = s_pad_ang - 90.0f;
    if (g < 0) g += 360.0f;
    lv_arc_set_bg_angles(s_paddle, (int32_t)(g - s_pad_half + 360) % 360,
                                   (int32_t)(g + s_pad_half) % 360);
}

/* For tests — where the ball is. Finding it by pixel also finds the paddle. */
void brk_debug_ball(float *x, float *y) { if (x) *x = s_bx; if (y) *y = s_by; }

/* Move the ball exactly one step (20 ms). brk_step decides how many to take. */
static void brk_phys(void)
{
    if (s_obs_dps > 0.0f) {
        s_obs_ang += s_obs_dps;
        if (s_obs_ang >= 360.0f) s_obs_ang -= 360.0f;
        for (int i = 0; i < OBS_N; i++) {
            if (!s_obs[i]) continue;
            float oa = (s_obs_ang + i * (360.0f / OBS_N)) * DEG2RAD;
            put(s_obs[i], CX + sinf(oa) * OBS_R, CY - cosf(oa) * OBS_R);
        }
    }

    s_bx += s_vx;
    s_by += s_vy;

    float dx = s_bx - CX, dy = s_by - CY;
    float r = sqrtf(dx * dx + dy * dy);
    float ang = atan2f(dx, -dy) / DEG2RAD;
    if (ang < 0) ang += 360;

    /* Bricks — rectangles, so the test is a box */
    bool hit_brick = false;
    for (int i = 0; i < s_brick_n; i++) {
        if (!s_alive[i]) continue;
        const lv_area_t *a = &s_brect[i];
        if (s_bx < a->x1 - BALL_R || s_bx > a->x2 + BALL_R ||
            s_by < a->y1 - BALL_R || s_by > a->y2 + BALL_R) continue;

        /* 🚨 A tough brick loses its outline on the first hit and stays put.
         * It still bounces the ball — if it did not, the ball would end up
         * trapped inside it. */
        if (s_hp[i] > 1) {
            s_hp[i]--;
            lv_obj_set_style_border_width(s_brick[i], 0, 0);
            blip(600, 30);
        } else {
            s_alive[i] = false;
            lv_obj_add_flag(s_brick[i], LV_OBJ_FLAG_HIDDEN);
            s_left_cnt--;
            blip(900, 40);
            lv_label_set_text_fmt(s_score, "L%d  %d  o%d", s_level + 1,
                                  s_brick_n - s_left_cnt, s_brk_life);
        }
        /* Flip whichever axis it came in on.
         * 🚨 **Push it out by the overlap.** Without that it stays inside a
         * tough brick (which does not disappear) and flips again next step —
         * two flips put it back on its original heading, which looks exactly
         * like passing straight through. */
        float o_r = a->x2 + BALL_R - s_bx;      /* distance out to the right */
        float o_l = s_bx - (a->x1 - BALL_R);
        float o_d = a->y2 + BALL_R - s_by;
        float o_u = s_by - (a->y1 - BALL_R);
        if (fminf(o_r, o_l) < fminf(o_d, o_u)) {
            s_vx = -s_vx;
            s_bx += (o_r < o_l) ? o_r : -o_l;
        } else {
            s_vy = -s_vy;
            s_by += (o_d < o_u) ? o_d : -o_u;
        }
        hit_brick = true;
        break;
    }
    if (lv_obj_has_flag(s_ball, LV_OBJ_FLAG_HIDDEN)) return;

    /* The spinning obstacle — circle against circle, so the test is short */
    if (s_obs_dps > 0.0f) {
        float hit = OBS_D * 0.5f + BALL_R;
        for (int i = 0; i < OBS_N; i++) {
            if (!s_obs[i]) continue;
            float oa = (s_obs_ang + i * (360.0f / OBS_N)) * DEG2RAD;
            float ox = CX + sinf(oa) * OBS_R, oy = CY - cosf(oa) * OBS_R;
            float ex = s_bx - ox, ey = s_by - oy;
            float e2 = ex * ex + ey * ey;
            if (e2 >= hit * hit || e2 < 0.01f) continue;
            float el = sqrtf(e2), nx = ex / el, ny = ey / el;
            float dp = s_vx * nx + s_vy * ny;
            if (dp < 0) { s_vx -= 2 * dp * nx; s_vy -= 2 * dp * ny; }
        /* 🚨 Push it out by the overlap here too. Otherwise it is still
         * inside next step, flips again, and ends up stuck to the bead. */
            s_bx = ox + nx * hit;
            s_by = oy + ny * hit;
            blip(700, 25);
            break;
        }
    }

    /* 🚨 Handle **one** collision per step. Flipping once off a brick and
     * again off the wall is two flips, which sends the ball back the way it
     * came. */
    if (!hit_brick && r > PADDLE_R) {
        bool bottom = (ang > PAD_MIN - 8 && ang < PAD_MAX + 8);
        float da = fabsf(ang - s_pad_ang);
        if (da > 180) da = 360 - da;

        if (!bottom) {                       /* top and sides just bounce */
            float nx = dx / r, ny = dy / r;
            float dp = s_vx * nx + s_vy * ny;
            s_vx -= 2 * dp * nx;
            s_vy -= 2 * dp * ny;
            s_bx = CX + nx * (PADDLE_R - 2);
            s_by = CY + ny * (PADDLE_R - 2);
        } else if (da < s_pad_half) {        /* caught by the paddle */
            float nx = dx / r, ny = dy / r;
            float dp = s_vx * nx + s_vy * ny;
            s_vx -= 2 * dp * nx;
            s_vy -= 2 * dp * ny;
            /* Where on the paddle it landed nudges the angle — that is what
             * makes it feel steerable. Angle only; speed is reset below. */
            float off = (ang - s_pad_ang) / s_pad_half;
            s_vx += off * 1.1f;

            /* Gain a little on each bounce, but always set the magnitude.
             * Otherwise reflections accumulate and it drifts faster or slower
             * on its own. */
            if (s_spd < s_spd0) s_spd = s_spd0;
            s_spd *= BALL_RAMP;
            if (s_spd > s_spdmax) s_spd = s_spdmax;
            float m = sqrtf(s_vx * s_vx + s_vy * s_vy);
            if (m > 0.01f) { s_vx = s_vx / m * s_spd; s_vy = s_vy / m * s_spd; }
            s_bx = CX + nx * (PADDLE_R - 3);
            s_by = CY + ny * (PADDLE_R - 3);
            blip(500, 30);
        } else if (r > 224) {                /* missed */
            lv_obj_add_flag(s_ball, LV_OBJ_FLAG_HIDDEN);
            stop_loop();
            /* 🚨 With a ball left, rebuild **the same level**. Only when they
             * run out does it go back to level 1 — that is where a run ends. */
            if (--s_brk_life > 0) {
                lv_label_set_text_fmt(s_score, "L%d  o%d", s_level + 1, s_brk_life);
            } else {
                s_brk_life = BRK_LIVES;
                s_level = 0;
                lv_label_set_text(s_score, "game over");
            }
            defer(brk_rebuild);
            return;
        }
    }
    put(s_ball, s_bx, s_by);
    if (s_left_cnt == 0) {
        stop_loop();
        if (s_level + 1 < BRK_LEVELS) {
            s_level++;
            lv_label_set_text_fmt(s_score, "L%d!  o%d", s_level + 1, s_brk_life);
            defer(brk_rebuild);
        } else {
            lv_label_set_text(s_score, "all clear!");
        }
    }
}

/* 🚨 This used to take exactly one step per timer call. LVGL timers arrive
 * late when drawing runs long, and sometimes arrive in a burst — which is
 * what made the ball stutter and then jump. Measure the time that actually
 * passed and spend it as whole steps. The step stays 20 ms, so collision
 * behaviour does not change with frame rate. */
/* ── tilting counts as input ──────────────────────────────────
 * 🚨 The launcher measures idleness with lv_display_get_inactive_time(),
 * which only counts touches. A game played by tilting never touches the
 * screen, so the display would go dark mid-game.
 * Stay awake only while the attitude is actually changing — put the badge
 * down and it sleeps as usual. 🚨 Call this only from tilt-driven boards. On
 * touch-driven ones the touch already counts and there is nothing to fix. */
static void tilt_is_input(float lat)
{
    static float last; static bool have;
    if (have && fabsf(lat - last) < 25.0f) return;   /* a shaky hand is not input */
    last = lat; have = true;
    lv_display_trigger_activity(NULL);
}

/* Steer the paddle by tilt. Whatever attitude it started in is the centre, so
 * it works lying down or standing up. */
static void tilt_drive_paddle(void)
{
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) return;
    /* 🚨 The IMU axes are rotated 90 degrees from the screen's (established
     * with the marble), and the paddle angle grows leftward (PAD_MIN 128 is
     * bottom right, PAD_MAX 232 bottom left). Both have to be lined up for
     * the paddle to go the way you tilt.
     * Tilt right and gy goes negative, the angle shrinks, the paddle goes right. */
    float lat = gy;
    tilt_is_input(lat);          /* only reachable in tilt mode */
    if (!s_tilt0_set) {
        s_tilt0 = lat; s_tilt0_set = true;
        ESP_LOGI("game", "tilt reference taken, lat=%.1f", lat);
    }
    float d = (lat - s_tilt0) * 0.11f;          /* tilt, as an angle */
    float want = (PAD_MIN + PAD_MAX) * 0.5f + d;
    if (want < PAD_MIN) want = PAD_MIN;
    if (want > PAD_MAX) want = PAD_MAX;
    /* Following it exactly follows the shake too. Lag slightly behind. */
    s_pad_ang += (want - s_pad_ang) * 0.35f;
    paddle_draw();
}

static void brk_step(lv_timer_t *t)
{
    (void)t;
    static uint32_t nstep;
    if ((nstep++ % 200) == 0)
        ESP_LOGI("game", "brick step %u, tilt mode=%d, paddle=%.0f",
                 (unsigned)nstep, (int)s_tiltmode, s_pad_ang);
    if (s_tiltmode) tilt_drive_paddle();
    /* With the display off there is no reason to move the ball. It used to
     * keep running fifty times a second behind the black veil, chirping at
     * every wall. */
    if (launcher_screen_is_off()) { port_tone_enable(false); port_tone_hold(false); s_brk_last = 0; return; }
    /* 🚨 Opening and closing the codec per sound effect loses the first one
     * entirely. Hold it open while a board is running — if it is already
     * open this does nothing. */
    port_tone_hold(true);

    uint32_t now = lv_tick_get();
    if (!s_brk_last) { s_brk_last = now; brk_phys(); return; }
    uint32_t el = now - s_brk_last;
    if (el > 200) el = 200;          /* after a long pause (app switch), no teleporting */
    s_brk_acc += el;
    s_brk_last = now;

    int steps = 0;
    while (s_brk_acc >= 20 && steps < 5) {
        s_brk_acc -= 20;
        brk_phys();
        steps++;
        /* Ball lost or board cleared means the loop already stopped — stop stepping */
        if (!s_loop || lv_obj_has_flag(s_ball, LV_OBJ_FLAG_HIDDEN)) break;
    }
}

static float s_grab_ang, s_grab_pad;

static void brk_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && consume_start_tap()) return;

    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);

    float a = atan2f((float)(p.x - CX), (float)(CY - p.y)) / DEG2RAD;
    if (a < 0) a += 360;

    if (code == LV_EVENT_PRESSED) { s_grab_ang = a; s_grab_pad = s_pad_ang; return; }
    if (code == LV_EVENT_RELEASED) {
        if (!s_loop && !s_pending_cb) {
            port_tone_enable(false);
            clear_board();

            brk_start();
        }
        return;
    }

    if (s_tiltmode) return;      /* while tilting, a finger does not take over */
    /* Turns like a dial, but cannot leave the bottom arc. */
    float d = a - s_grab_ang;
    while (d > 180)  d -= 360;
    while (d < -180) d += 360;
    s_pad_ang = s_grab_pad + d * 1.5f;
    if (s_pad_ang < PAD_MIN) s_pad_ang = PAD_MIN;
    if (s_pad_ang > PAD_MAX) s_pad_ang = PAD_MAX;
    paddle_draw();
}

/* For tests — start brick breaker immediately, without waiting for a tap.
 * Needed to measure how many milliseconds the timer actually takes while a
 * game is running, which is what sets the ball speed. */
/* For tests — expose the captured "level" reference next to the live value.
 * 🚨 Taking a reference right after waking the IMU puts it somewhere wrong,
 * and all you see is a paddle that will not move. Only the numbers show it. */
void games_debug_tilt0(float *base, int *set)
{
    if (base) *base = s_tilt0;
    if (set)  *set  = s_tilt0_set ? 1 : 0;
}

/* For tests — open the planets directly, skipping the chooser */
void games_debug_play_orb(void)
{
    clear_board();
    launcher_handle_show(true);
    s_loop = orb_start(s_root, ORB_EARTH);
}



/* For tests — start brick breaker immediately, without waiting for a tap.
 * Needed to measure how many milliseconds the timer actually takes while a
 * game is running, which is what sets the ball speed. */
void games_debug_play_bricks(void);   /* defined below */

/* For tests — start brick breaker at a chosen level. Without this door there
 * is no way to walk all five without a person. */
void games_debug_brk_level(int lv)
{
    s_level = (lv < 0) ? 0 : (lv >= BRK_LEVELS ? BRK_LEVELS - 1 : lv);
    games_debug_play_bricks();
}

/* For tests — check from outside that the board matches the level table. */
void games_debug_brk_info(int *level, int *n, int *left, int *tough,
                          float *padh, float *obs)
{
    if (level) *level = s_level;
    if (n)     *n     = s_brick_n;
    if (left)  *left  = s_left_cnt;
    if (padh)  *padh  = s_pad_half;
    if (obs)   *obs   = s_obs_dps;
    if (tough) {
        int t = 0;
        for (int i = 0; i < s_brick_n; i++) if (s_hp[i] > 1) t++;
        *tough = t;
    }
}

void games_debug_play_bricks(void)
{
    clear_board();
    launcher_handle_show(false);
    brk_start();
    if (s_ready_lbl) { lv_obj_delete(s_ready_lbl); s_ready_lbl = NULL; }
    s_pending_cb = NULL;
    s_brk_last = s_brk_acc = 0;
    s_loop = lv_timer_create(brk_step, 20, NULL);
}

/* Rebuild the board on a level change or a miss. 🚨 Always through defer —
 * lv_obj_clean from inside a timer callback deletes the object running it. */
static void brk_rebuild(void)
{
    clear_board();
    launcher_handle_show(false);
    brk_start();
}

void brk_start(void)
{
    /* The handle is hidden during a game, so starting in the middle does not
     * overlap it. Paddle at bottom centre, ball just above. */
    if (s_level < 0 || s_level >= BRK_LEVELS) s_level = 0;
    const brk_level_t *L = &BRK_LV[s_level];
    s_spd0     = L->spd0;
    s_spdmax   = L->spdmax;
    s_pad_half = L->pad_half;
    s_obs_dps  = L->obs_dps;
    s_obs_ang  = 0.0f;

    s_pad_ang = (PAD_MIN + PAD_MAX) / 2;
    s_bx = CX;
    s_by = CY + (PADDLE_R - 40);
    s_brk_last = s_brk_acc = 0;                      /* banked time restarts too */
    s_spd = s_spd0;                                  /* fresh speed each board */
    s_vx = s_spd0 * 0.6f; s_vy = -s_spd0 * 0.8f;     /* a 3:4 heading */
    s_brick_n = 0;

    lv_obj_t *pad = lv_obj_create(s_root);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 466, 466);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_RELEASED, NULL);

    /* Three or four rows at the top. Each row measures what the circle allows
     * and fits its count and width to it. 🚨 Four rows start **higher**, not
     * lower. Hanging an extra row underneath brings the bottom row down to
     * the middle of the screen and cuts the ball's travel to the paddle,
     * which does not add difficulty — it just removes reaction time. */
    static const uint32_t COL[BRK_ROWS] = { 0x7FB0FF, 0x5BD48A, 0xE0A33A, 0xC98BE0 };
    int rows = (L->rows < 3) ? 3 : (L->rows > BRK_ROWS ? BRK_ROWS : L->rows);
    /* 🚨 The buttons moved inward to clear the wall (GBTN_IN_DY = -174, so
     * y 42..76). Rows start below that. The old value (60) put the buttons on
     * top of the first row. */
    int top  = BRK_TOP;
    for (int row = 0; row < rows; row++) {
        int y = top + row * (BRK_H + BRK_GAP);
        /* 🚨 **Measuring from the bottom edge was wrong.** Every row is above
         * centre, so the corner that meets the circle first is the **top**
         * one. Measured from the bottom edge, the upper corners of the end
         * bricks stick well outside — the first row reached r=225 against a
         * wall at 206. It only became visible once the wall was drawn
         * ("the bricks overlap it"). Measure from whichever edge is further. */
        float fa1 = fabsf((float)y - 233.0f);
        float fa2 = fabsf((float)(y + BRK_H) - 233.0f);
        float far = fa1 > fa2 ? fa1 : fa2;
        float v = BRK_FIT * BRK_FIT - far * far;
        int half = v <= 0 ? 0 : (int)sqrtf(v);
        int total = half * 2;
        int n = total / 62;
        if (n > BRK_MAX) n = BRK_MAX;
        if (n < 1) continue;
        int bw = (total - BRK_GAP * (n - 1)) / n;
        int x0 = 233 - total / 2;

        for (int k = 0; k < n; k++) {
            /* Each level rearranges the bricks. The point is a different
             * board, not the same board faster. */
            bool skip = false;
            if (L->pattern == 1)      skip = ((row + k) & 1);          /* checker */
            else if (L->pattern == 2) skip = (n >= 5 && k == n / 2);   /* one gap in the middle */
            if (skip) continue;

            int x = x0 + k * (bw + BRK_GAP);
            lv_obj_t *b = lv_obj_create(s_root);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, bw, BRK_H);
            lv_obj_set_pos(b, x, y);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(COL[row]), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            /* Tough bricks get a white outline and lose it on the first hit —
             * only the look changes, the geometry stays put. */
            s_hp[s_brick_n] = (row < L->tough_rows) ? 2 : 1;
            if (s_hp[s_brick_n] > 1) {
                /* 🚨 After remove_style_all an object has border_side NONE,
                 * so width and colour alone draw nothing. The sides have to
                 * be switched on. */
                lv_obj_set_style_border_side(b, LV_BORDER_SIDE_FULL, 0);
                lv_obj_set_style_border_width(b, 3, 0);
                lv_obj_set_style_border_color(b, lv_color_white(), 0);
                lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
            }

            s_brick[s_brick_n] = b;
            s_brect[s_brick_n].x1 = x;
            s_brect[s_brick_n].x2 = x + bw;
            s_brect[s_brick_n].y1 = y;
            s_brect[s_brick_n].y2 = y + BRK_H;
            s_alive[s_brick_n] = true;
            s_brick_n++;
        }
    }
    s_left_cnt = s_brick_n;

    /* 🚨 You could not see where the wall was and where the gap was. With
     * only the paddle drawn, everything else looked equally empty, so
     * bouncing off the top and falling out the bottom both looked like luck.
     * Draw a thin line over **exactly the solid part**. That is the same
     * range the collision uses — where `bottom` is false in brk_phys, which
     * is 240 degrees clockwise from 12 o'clock, for 120 degrees. LVGL puts 0
     * at 3 o'clock, so subtracting 90 gives 150 -> 30. */
    lv_obj_t *wall = lv_arc_create(s_root);
    lv_obj_remove_style_all(wall);
    lv_obj_remove_flag(wall, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wall, BRK_WALL_D, BRK_WALL_D);
    lv_obj_center(wall);
    lv_arc_set_bg_angles(wall, (int32_t)(PAD_MAX + 8 - 90), (int32_t)(PAD_MIN - 8 - 90));
    lv_arc_set_value(wall, 0);
    lv_obj_set_style_arc_width(wall, BRK_WALL_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(wall, lv_color_hex(0x3A3A46), LV_PART_MAIN);

    /* 🚨 The paddle uses the same inner face. At the old size (422) that face
     * was at r=195, so the ball (centre 198, radius 8) bounced while half
     * buried in it. */
    s_paddle = lv_arc_create(s_root);
    lv_obj_remove_style_all(s_paddle);
    lv_obj_remove_flag(s_paddle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_paddle, BRK_PAD_D, BRK_PAD_D);
    lv_obj_center(s_paddle);
    lv_obj_set_style_arc_width(s_paddle, BRK_PAD_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_paddle, lv_color_white(), LV_PART_MAIN);
    lv_arc_set_value(s_paddle, 0);
    paddle_draw();

    if (s_obs_dps > 0.0f) {
        for (int i = 0; i < OBS_N; i++) {
            s_obs[i] = dot(s_root, OBS_D, 0xE05070);
            float oa = (s_obs_ang + i * (360.0f / OBS_N)) * DEG2RAD;
            put(s_obs[i], CX + sinf(oa) * OBS_R, CY - cosf(oa) * OBS_R);
        }
    }

    s_ball = dot(s_root, (int)(BALL_R * 2), 0xFFFFFF);
    /* 🚨 Position it or LVGL leaves a new object at (0,0) — the ball sat in
     * the top-left corner for as long as "tap to start" was showing, and only
     * jumped into place on the first step. Pinball placed its ball on
     * creation; this was the one that did not. */
    put(s_ball, s_bx, s_by);
    s_score = make_score();
    lv_label_set_text_fmt(s_score, "L%d  0  o%d", s_level + 1, s_brk_life);
    add_back_xy(do_back, -GBTN_IN_DX, GBTN_IN_DY);
    add_tilt_btn_xy(GBTN_IN_DX, GBTN_IN_DY);
    arm_start(brk_step, 20);
}

/* ── 3) marble maze ──────────────────────────────────────────
 * Tilt and the marble rolls. Drop it in the hole and the next hole appears
 * somewhere else. With no IMU, the simulator uses the finger position instead. */

static lv_obj_t *s_marble, *s_hole;
static float     s_mx, s_my, s_mvx, s_mvy, s_hx, s_hy;
static int       s_got;
static float     s_fake_gx, s_fake_gy;

static void new_hole(void)
{
    float a = (float)(rand() % 360) * DEG2RAD;
    float r = 50 + (float)(rand() % 120);       /* keep it off the very edge */
    s_hx = CX + cosf(a) * r;
    s_hy = CY + sinf(a) * r;
    put(s_hole, s_hx, s_hy);
}

/* For tests — where the marble is. Needed to check the tilt axes are not flipped. */
void mz_debug_ball(float *x, float *y) { if (x) *x = s_mx; if (y) *y = s_my; }

static void maze_step(lv_timer_t *t)
{
    (void)t;
    /* With the display off there is no reason to roll it. It used to keep
     * running fifty times a second behind the black veil, chirping at every wall. */
    if (launcher_screen_is_off()) { port_tone_enable(false); port_tone_hold(false); return; }
    port_tone_hold(true);
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) { gx = s_fake_gx; gy = s_fake_gy; }
    tilt_is_input(gy);           /* the marble is played by tilt alone */

    /* Subtract the attitude at the start, leaving only "how far from there".
     * That makes the starting pose level, lying down or held up. */
    if (s_g0_set) { gx -= s_g0x; gy -= s_g0y; }

    /* mg into acceleration. The numbers are large, so this scales them well down.
     * On signs: flipping the display 180 degrees with MADCTL in an earlier
     * revision (for how the strap sits) also flipped how the IMU axes map to
     * the screen. The IMU is bolted to the board and does not rotate with the
     * display, so the correction is made here by hand. */
    /* 🚨 It was not the signs — the axes were swapped outright. The IMU sits
     * on the board rotated 90 degrees from the screen, so flipping signs just
     * kept it rolling sideways (wrong twice before this was understood).
     *
     * Worked backwards from four observations of the broken version:
     *     tilt right   -> went down    => gy drives the screen's vertical
     *     tilt left    -> went up
     *     tilt forward -> went left    => gx drives the screen's horizontal
     *     tilt back    -> went right
     * So gx belongs to vertical and gy to horizontal. Swap them.
     *
     * After: right -> right, left -> left, forward -> up, back -> down.
     * Tip a bowl and the contents go that way, which is what anyone expects. */
    s_mvx -= gy * 0.00032f;
    s_mvy -= gx * 0.00032f;
    s_mvx *= 0.965f;                 /* rolling friction, firm enough to keep it off the rim */
    s_mvy *= 0.965f;

    /* Too fast and it jumps over the hole. Cap it. */
    float sp = sqrtf(s_mvx * s_mvx + s_mvy * s_mvy);
    if (sp > 7.0f) { s_mvx = s_mvx / sp * 7.0f; s_mvy = s_mvy / sp * 7.0f; }
    s_mx += s_mvx;
    s_my += s_mvy;

    /* Bounce off the rim */
    float dx = s_mx - CX, dy = s_my - CY;
    float r = sqrtf(dx * dx + dy * dy);
    if (r > 202) {
        float nx = dx / r, ny = dy / r;
        float dp = s_mvx * nx + s_mvy * ny;
        s_mvx = (s_mvx - 2 * dp * nx) * 0.45f;
        s_mvy = (s_mvy - 2 * dp * ny) * 0.45f;
        s_mx = CX + nx * 201;
        s_my = CY + ny * 201;
    }
    put(s_marble, s_mx, s_my);

    float hdx = s_mx - s_hx, hdy = s_my - s_hy;
    if (hdx * hdx + hdy * hdy < 30 * 30) {      /* a generous hole */
        s_got++;
        lv_label_set_text_fmt(s_score, "%d", s_got);
        blip(1200, 80);
        s_mvx = s_mvy = 0;
        new_hole();
    }
}

static void maze_touch(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED && consume_start_tap()) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    /* In the simulator it drifts toward the finger. gx/gy were swapped above, so swap here too */
    s_fake_gx = -(p.y - CY) * 4.0f;
    s_fake_gy = -(p.x - CX) * 4.0f;
}

void maze_start(void)
{
    s_mx = CX; s_my = CY;
    s_mvx = s_mvy = 0;
    s_got = 0;

    lv_obj_t *ring = lv_arc_create(s_root);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, 420, 420);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 0);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x2A2A32), LV_PART_MAIN);

    lv_obj_t *pad = lv_obj_create(s_root);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 466, 466);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, maze_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(pad, maze_touch, LV_EVENT_PRESSED, NULL);

    s_hole = dot(s_root, 34, 0x3A3A44);
    s_marble = dot(s_root, 22, 0xE8E8F0);
    put(s_marble, s_mx, s_my);   /* same reason as the brick ball — otherwise (0,0) */
    new_hole();
    s_score = make_score();
    lv_label_set_text(s_score, "0");
    add_back();
    arm_start(maze_step, 25);
}

/* ── 4) pinball ──────────────────────────────────────────────
 * The round screen is the table. A pinball playfield is a round dish to begin
 * with, and a rectangle would be the odd choice — like the ring paddle in
 * brick breaker, this is a case where round makes the rules better.
 *
 * 🚨 It is not a real table shrunk down. Fit eight or so elements in and each
 * one is 20-40 px (2-4 mm) and they stop being distinguishable. There are
 * six, and they are large: three bumpers, two flippers, one drain.
 *
 * 🚨 A game is something your hands stay on. So that this is not "launch it
 * and watch", the flippers are not the only input — **tilting the badge
 * nudges the ball**. That is the real technique in pinball, and here it is an
 * actual tilt. */

static bool pb_sub(const float *om, bool *kicked);

static void pb_flip_draw(int i)
{
    float a = s_flip[i].ang * DEG2RAD;
    s_flip_pt[i][0].x = (lv_value_precise_t)s_flip[i].px;
    s_flip_pt[i][0].y = (lv_value_precise_t)s_flip[i].py;
    s_flip_pt[i][1].x = (lv_value_precise_t)(s_flip[i].px + cosf(a) * PB_FLIP_L);
    s_flip_pt[i][1].y = (lv_value_precise_t)(s_flip[i].py + sinf(a) * PB_FLIP_L);
    lv_line_set_points(s_flip_obj[i], s_flip_pt[i], 2);
}

/* Did it hit a flipper? Find the nearest point on the segment and reflect
 * about that normal.
 * 🚨 If the flipper is **moving, its speed has to be added**. Without that the
 * ball merely bounces off and drops, and it is not pinball — being hit up the
 * table is the whole thing. */
static bool pb_flip_hit(int i, float omega)
{
    pb_flip_t *f = &s_flip[i];
    float a = f->ang * DEG2RAD, ex = cosf(a), ey = sinf(a);
    float rx = s_pb_x - f->px, ry = s_pb_y - f->py;
    float t = rx * ex + ry * ey;
    if (t < 0) t = 0;
    if (t > PB_FLIP_L) t = PB_FLIP_L;
    float cx = f->px + ex * t, cy = f->py + ey * t;
    float dx = s_pb_x - cx, dy = s_pb_y - cy;
    float d2 = dx * dx + dy * dy;
    float hit = PB_BR + PB_FLIP_W * 0.5f;
    if (d2 >= hit * hit) return false;
    float d = sqrtf(d2);
    float nx, ny;
    if (d < 0.01f) { nx = 0; ny = -1; } else { nx = dx / d; ny = dy / d; }
    s_pb_x = cx + nx * hit;
    s_pb_y = cy + ny * hit;
    float dp = s_pb_vx * nx + s_pb_vy * ny;
    if (dp < 0) { s_pb_vx -= 1.75f * dp * nx; s_pb_vy -= 1.75f * dp * ny; }
    if (omega != 0.0f) {
        float w = omega * DEG2RAD;              /* radians per step */
        s_pb_vx += -ey * w * t;
        s_pb_vy +=  ex * w * t;
    }
    blip(420, 24);
    return true;
}

static void pb_lose_ball(void)
{
    s_pb_left--;
    blip(180, 120);
    if (s_pb_left <= 0) {
        lv_label_set_text_fmt(s_score, "%d", s_pb_pts);
        lv_obj_add_flag(s_pb_ball, LV_OBJ_FLAG_HIDDEN);
        stop_loop();
        defer(pb_rebuild);
        return;
    }
    /* The next ball drops in from the top */
    s_pb_x = CX + 96.0f; s_pb_y = CY - 150.0f;
    s_pb_vx = -1.2f; s_pb_vy = 0.6f;
    lv_label_set_text_fmt(s_score, "%d  o%d", s_pb_pts, s_pb_left);
}

static void pb_phys(void)
{
    /* 🚨 Tilt is the table's slope. There is one measured axis mapping on
     * this badge and the water and marble already use it:
     *     screen right = -ay      screen down = -ax
     * Pinball was the one using `+gy` for right, so **left and right were
     * reversed** ("tilt left and it goes right"), and it never looked at the
     * vertical at all, so tilting forward and back did nothing.
     * 🚨 It also used to take the attitude at launch as zero (s_pb_tilt0).
     * A table's slope is absolute, not relative to how you hold it — starting
     * while tilted made that tilt into "level". Removed. */
    float gx, gy;
    if (port_imu_accel(&gx, &gy)) {
        tilt_is_input(gy);
        s_pb_vx += -gy * PB_TILT;
        s_pb_vy += -gx * PB_TILT;
    }

    /* 🚨 Keep the built-in slope. On tilt alone the ball floats when the
     * badge is flat, and a real table is always tipped toward you. */
    s_pb_vy += PB_GRAV;
    s_pb_vx *= PB_DAMP;
    s_pb_vy *= PB_DAMP;
    float sp = sqrtf(s_pb_vx * s_pb_vx + s_pb_vy * s_pb_vy);
    if (sp > PB_MAXV) { s_pb_vx = s_pb_vx / sp * PB_MAXV; s_pb_vy = s_pb_vy / sp * PB_MAXV; }

    /* Move the flippers toward their target. How far they turned this step is the feel. */
    float om[2];
    for (int i = 0; i < 2; i++) {
        float want = s_flip[i].on ? s_flip[i].up : s_flip[i].rest;
        float d = want - s_flip[i].ang;
        if (d >  PB_FLIP_STEP) d =  PB_FLIP_STEP;
        if (d < -PB_FLIP_STEP) d = -PB_FLIP_STEP;
        om[i] = d;
    }
    /* 🚨 The speed a flipper imparts (om) is **per step**, so it must not be
     * divided across the sub-steps. Apply it once per step — applying it in
     * each sub-step launches the ball three times as hard. */
    bool kicked[2] = { false, false };
    for (int k = 0; k < PB_SUB; k++) {
        for (int i = 0; i < 2; i++)
            if (om[i] != 0.0f) { s_flip[i].ang += om[i] / PB_SUB; pb_flip_draw(i); }
        if (!pb_sub(om, kicked)) return;
    }
    put(s_pb_ball, s_pb_x, s_pb_y);
}

/* One sub-step: move the ball a little and test where it lands.
 * false means the ball drained, and the step ends there. */
static bool pb_sub(const float *om, bool *kicked)
{
    s_pb_x += s_pb_vx / PB_SUB;
    s_pb_y += s_pb_vy / PB_SUB;

    /* Bumpers — score and a hard kick away */
    for (int i = 0; i < PB_BUMP_N; i++) {
        float dx = s_pb_x - s_pb_bx[i], dy = s_pb_y - s_pb_by[i];
        float d2 = dx * dx + dy * dy, hit = PB_BUMP_R + PB_BR;
        if (d2 >= hit * hit || d2 < 0.01f) continue;
        float d = sqrtf(d2), nx = dx / d, ny = dy / d;
        s_pb_x = s_pb_bx[i] + nx * hit;
        s_pb_y = s_pb_by[i] + ny * hit;
        float dp = s_pb_vx * nx + s_pb_vy * ny;
        s_pb_vx -= (1.0f + PB_BUMP_E) * dp * nx;
        s_pb_vy -= (1.0f + PB_BUMP_E) * dp * ny;
        s_pb_pts += 10;
        lv_label_set_text_fmt(s_score, "%d  o%d", s_pb_pts, s_pb_left);
        blip(1100, 30);
        break;
    }

    /* Targets — knocked flat, worth a lot. Flatten all four and they all stand back up. */
    for (int i = 0; i < PB_TGT_N; i++) {
        if (!s_pb_tup[i]) continue;
        float tdx = s_pb_x - s_pb_tx[i], tdy = s_pb_y - s_pb_ty[i];
        float td2 = tdx * tdx + tdy * tdy, th = PB_TGT_R + PB_BR;
        if (td2 >= th * th || td2 < 0.01f) continue;
        float td = sqrtf(td2), tnx = tdx / td, tny = tdy / td;
        s_pb_x = s_pb_tx[i] + tnx * th;
        s_pb_y = s_pb_ty[i] + tny * th;
        float tdp = s_pb_vx * tnx + s_pb_vy * tny;
        s_pb_vx -= 1.6f * tdp * tnx;
        s_pb_vy -= 1.6f * tdp * tny;
        s_pb_tup[i] = false;
        lv_obj_add_flag(s_pb_tgt[i], LV_OBJ_FLAG_HIDDEN);
        s_pb_pts += 50;
        int up = 0;
        for (int k = 0; k < PB_TGT_N; k++) if (s_pb_tup[k]) up++;
        if (up == 0) {                     /* all down — bonus, and reset them */
            s_pb_pts += 200;
            for (int k = 0; k < PB_TGT_N; k++) {
                s_pb_tup[k] = true;
                lv_obj_remove_flag(s_pb_tgt[k], LV_OBJ_FLAG_HIDDEN);
            }
            blip(1600, 90);
        } else {
            blip(1400, 40);
        }
        lv_label_set_text_fmt(s_score, "%d  o%d", s_pb_pts, s_pb_left);
        break;
    }

    for (int i = 0; i < 2; i++)
        if (pb_flip_hit(i, kicked[i] ? 0.0f : om[i])) { kicked[i] = true; break; }

    /* The wall, open only at the bottom centre. Go out there and the ball is lost. */
    float dx = s_pb_x - CX, dy = s_pb_y - CY;
    float r = sqrtf(dx * dx + dy * dy);
    if (r > PB_WALL) {
        if (dy > 0 && fabsf(dx) < PB_DRAIN_X) { pb_lose_ball(); return false; }
        float nx = dx / r, ny = dy / r;
        float dp = s_pb_vx * nx + s_pb_vy * ny;
        s_pb_vx -= (1.0f + PB_WALL_E) * dp * nx;
        s_pb_vy -= (1.0f + PB_WALL_E) * dp * ny;
        s_pb_x = CX + nx * PB_WALL;
        s_pb_y = CY + ny * PB_WALL;
    }
    return true;
}

static void pb_step(lv_timer_t *t)
{
    (void)t;
    if (launcher_screen_is_off()) { port_tone_enable(false); port_tone_hold(false); s_pb_last = 0; return; }
    port_tone_hold(true);
    uint32_t now = lv_tick_get();
    if (!s_pb_last) { s_pb_last = now; pb_phys(); return; }
    uint32_t el = now - s_pb_last;
    if (el > 200) el = 200;
    s_pb_acc += el;
    s_pb_last = now;
    int steps = 0;
    while (s_pb_acc >= 20 && steps < 5) {
        s_pb_acc -= 20;
        pb_phys();
        steps++;
        if (!s_loop) break;
    }
}

/* Left half of the screen is the left flipper, right half the right. Two fingers work. */
static void pb_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && consume_start_tap()) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    bool down = (code != LV_EVENT_RELEASED);
    int i = (p.x < CX) ? 0 : 1;
    if (down) s_flip[i].on = true;
    else      s_flip[0].on = s_flip[1].on = false;
}

static void pb_rebuild(void)
{
    clear_board();
    launcher_handle_show(false);
    pb_start();
}

void pb_start(void)
{
    s_pb_pts = 0;
    s_pb_left = PB_BALLS;
    s_pb_last = s_pb_acc = 0;
    s_pb_x = CX + 96.0f; s_pb_y = CY - 150.0f;
    s_pb_vx = -1.2f; s_pb_vy = 0.6f;

    /* 🚨 The drawn wall and the collision disagreed ("the bouncing is off").
     * A circle of diameter 424 has its inner face at r=206, which is where the
     * ball's **centre** stops — so a ball of radius 6.5 covers the wall
     * entirely and pokes out the far side. What you see is a ball going
     * through the wall. Put the inner face where the ball actually touches
     * (PB_WALL + PB_BR). For lv_arc that face is size/2 - line width. */
    lv_obj_t *ring = lv_arc_create(s_root);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, PB_RING_D, PB_RING_D);
    lv_obj_center(ring);
    /* 🚨 The drain has to be visible too. Drawing the full 360 while only the
     * bottom centre let the ball out made it look like it vanished through
     * solid wall. Leave a gap exactly as wide as the opening —
     * half-angle = asin(drain width / wall radius). lv_arc has 0 at 3 o'clock,
     * clockwise. */
    float ga = asinf(PB_DRAIN_X / PB_WALL) / DEG2RAD;
    lv_arc_set_bg_angles(ring, (int32_t)(90.0f + ga), (int32_t)(90.0f - ga));
    lv_arc_set_value(ring, 0);
    lv_obj_set_style_arc_width(ring, PB_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x3A3A46), LV_PART_MAIN);

    lv_obj_t *pad = lv_obj_create(s_root);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 466, 466);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, pb_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(pad, pb_touch, LV_EVENT_RELEASED, NULL);

    /* 🚨 Three big bumpers left the table bare ("the level is too simple").
     * The screen cannot grow, so **everything shrank to make room** — ball
     * 9 -> 6.5, bumpers 30 -> 21. That bought five bumpers and four targets. */
    static const float BX[PB_BUMP_N] = {   0, -78,  78, -46,  46 };
    static const float BY[PB_BUMP_N] = { -128, -58, -58,  26,  26 };
    static const uint32_t BC[PB_BUMP_N] = { 0xE0B33A, 0x5BD48A, 0x7FB0FF,
                                            0xE06FA0, 0x9A7BE0 };
    for (int i = 0; i < PB_BUMP_N; i++) {
        s_pb_bx[i] = CX + BX[i];
        s_pb_by[i] = CY + BY[i];
        s_pb_bump[i] = dot(s_root, (int)(PB_BUMP_R * 2), BC[i]);
        put(s_pb_bump[i], s_pb_bx[i], s_pb_by[i]);
    }

    /* Four targets, two per upper lane. You have to send the ball up there to hit them. */
    static const float TX[PB_TGT_N] = { -150, -150,  150,  150 };
    static const float TY[PB_TGT_N] = {  -78,  -34,  -78,  -34 };
    for (int i = 0; i < PB_TGT_N; i++) {
        s_pb_tx[i] = CX + TX[i];
        s_pb_ty[i] = CY + TY[i];
        s_pb_tup[i] = true;
        s_pb_tgt[i] = dot(s_root, (int)(PB_TGT_R * 2), 0xFFD24A);
        put(s_pb_tgt[i], s_pb_tx[i], s_pb_ty[i]);
    }

    /* Two flippers: resting they point down and inward; raised they hit up. */
    s_flip[0] = (pb_flip_t){ CX - PB_FLIP_PX, CY + PB_FLIP_PY,  24.0f, -34.0f,  24.0f, false };
    s_flip[1] = (pb_flip_t){ CX + PB_FLIP_PX, CY + PB_FLIP_PY, 156.0f, 214.0f, 156.0f, false };
    for (int i = 0; i < 2; i++) {
        s_flip_obj[i] = lv_line_create(s_root);
        lv_obj_remove_style_all(s_flip_obj[i]);
        lv_obj_set_pos(s_flip_obj[i], 0, 0);
        lv_obj_set_style_line_width(s_flip_obj[i], (int)PB_FLIP_W, 0);
        lv_obj_set_style_line_color(s_flip_obj[i], lv_color_white(), 0);
        lv_obj_set_style_line_rounded(s_flip_obj[i], true, 0);
        pb_flip_draw(i);
    }

    s_pb_ball = dot(s_root, (int)(PB_BR * 2), 0xFFFFFF);
    put(s_pb_ball, s_pb_x, s_pb_y);
    s_score = make_score();
    /* 🚨 The shared score position (+168) sits between the flippers — exactly
     * where the ball drains. Text over the drain hides the moment you lose it. */
    lv_obj_align(s_score, LV_ALIGN_CENTER, 0, 116);
    lv_label_set_text_fmt(s_score, "0  o%d", s_pb_left);
    add_back_xy(do_back, -GBTN_IN_DX, GBTN_IN_DY);
    arm_start(pb_step, 20);
}

/* For tests — start pinball directly */
void games_debug_play_pinball(void)
{
    clear_board();
    launcher_handle_show(false);
    pb_start();
    if (s_ready_lbl) { lv_obj_delete(s_ready_lbl); s_ready_lbl = NULL; }
    s_pending_cb = NULL;
    s_pb_last = s_pb_acc = 0;
    s_loop = lv_timer_create(pb_step, 20, NULL);
}

/* For tests — where the ball is and how many are left */
void pb_debug(float *x, float *y, int *pts, int *left)
{
    if (x)    *x    = s_pb_x;
    if (y)    *y    = s_pb_y;
    if (pts)  *pts  = s_pb_pts;
    if (left) *left = s_pb_left;
}

/* ── menu ────────────────────────────────────────────────────── */

static int s_pick;

static void do_pick(void)
{
    clear_board();
    /* In the games (bricks, marble, bubble wrap) the whole screen is the
     * control surface, so the handle would steal input. Water and the planets
     * leave the bottom empty, so the handle can stay — and without it the only
     * way home would be the power button, which is worse. */
    launcher_handle_show(false);   /* games own the whole screen */
    switch (s_pick) {
        /* Chosen from the menu: level 1, and a fresh set of balls */
        case 0: s_level = 0; s_brk_life = BRK_LIVES; brk_start(); break;
        case 1: pb_start();   break;
        case 2: maze_start(); break;
        default: pop_start(); break;
    }
}

static void pick_cb(lv_event_t *e)
{
    /* Rebuilding here would free the very button being pressed -> defer */
    s_pick = (int)(intptr_t)lv_event_get_user_data(e);
    defer(do_pick);
}


/* ── bubble wrap ─────────────────────────────────────────────
 * A fidget toy. Leaving this badge on as a digital name tag does not survive
 * the battery (the display is most of the draw), so the shape this aims for
 * is "take it out, fiddle, put it away". No score and no rules — press one
 * and it pops, pop them all and they come back.
 * One 10 Hz timer, and all it does is check whether the display went off. */
static uint32_t pop_rnd(void)
{
    s_pop_seed ^= s_pop_seed << 13;
    s_pop_seed ^= s_pop_seed >> 17;
    s_pop_seed ^= s_pop_seed << 5;
    return s_pop_seed;
}

static void pop_face(int i, bool popped)
{
    lv_obj_t *b = s_pop[i];
    if (!b) return;
    if (popped) {
        lv_obj_set_size(b, POP_D - 24, POP_D - 24);
        lv_obj_set_style_radius(b, (POP_D - 24) / 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x23262E), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
    } else {
        lv_obj_set_size(b, POP_D, POP_D);
        lv_obj_set_style_radius(b, POP_D / 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xA8C2E0), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x53709A), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
    }
}

static void pop_paint_count(void)
{
    if (s_pop_lbl) lv_label_set_text_fmt(s_pop_lbl, "%d", s_pop_left);
}

static void pop_refill(void)
{
    for (int i = 0; i < s_pop_n; i++) { s_popped[i] = 0; pop_face(i, false); }
    s_pop_left = s_pop_n;
    pop_paint_count();
    blip(520, 130);
}

static void pop_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_pop_n || s_popped[i]) return;
    s_popped[i] = 1;
    s_pop_left--;
    pop_face(i, true);
    pop_paint_count();
    /* Real bubble wrap does not sound the same twice either */
    blip(760 + (pop_rnd() % 620), 22);
    if (s_pop_left == 0) s_refill_in = 7;    /* refill in about 0.8 s */
}

/* Hold to refill without popping them all */
static void pop_long_cb(lv_event_t *e)
{
    (void)e;
    pop_refill();
}

static void pop_step(lv_timer_t *t)
{
    /* 🔋 No reason to wake at 10 Hz behind a dark screen. Slow down.
     * (App timers keep running with the display off — you have to do this.) */
    if (launcher_screen_is_off()) {
        port_tone_enable(false);
        port_tone_hold(false);
        /* 🚨 Changing the period makes lv_timer_handler restart forever.
         * Leave it and act on every 16th call instead. */
        static uint8_t skip;
        if (++skip % 16) return;
        return;
    }
    port_tone_hold(true);        /* does nothing if it is already open */
    if (s_refill_in > 0 && --s_refill_in == 0) pop_refill();
}

void pop_start(void)
{
    s_pop_n = 0;
    s_refill_in = 0;
    lv_obj_t *field = lv_obj_create(s_root);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, 466, 466);
    lv_obj_center(field);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(field, pop_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* A hex layout fills a round screen evenly. Every other row shifts half a cell. */
    const int step_x = POP_D + 4, step_y = 70;
    for (int row = -2; row <= 2 && s_pop_n < POP_MAX; row++) {
        int y = CY + row * step_y;
        int off = (row & 1) ? step_x / 2 : 0;
        for (int col = -2; col <= 2 && s_pop_n < POP_MAX; col++) {
            int x = CX + col * step_x + off;
            int dx = x - CX, dy = y - CY;
            if (dx * dx + dy * dy > POP_R * POP_R) continue;

            lv_obj_t *b = lv_obj_create(field);
            lv_obj_remove_style_all(b);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            s_pop[s_pop_n] = b;
            s_popped[s_pop_n] = 0;
    /* Popping on press, not on release, is what makes it feel right */
            lv_obj_add_event_cb(b, pop_cb, LV_EVENT_PRESSED, (void *)(intptr_t)s_pop_n);
            pop_face(s_pop_n, false);
            lv_obj_set_pos(b, x - POP_D / 2, y - POP_D / 2);
            s_pop_n++;
        }
    }
    s_pop_left = s_pop_n;

    s_pop_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_pop_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_pop_lbl, lv_color_hex(0x6A7486), 0);
    lv_obj_align(s_pop_lbl, LV_ALIGN_CENTER, 0, 196);
    pop_paint_count();

    /* 🚨 This was the one of the four with no back button. The other three
     * call add_back and this function simply did not — the kind of omission
     * you cannot see by looking. */
    add_back();

    s_loop = lv_timer_create(pop_step, 120, NULL);
}

static void show_menu(void)
{
    /* A tall list gets clipped top and bottom on a round screen. Rows of two fit.
     * Only the games are listed — water and the planets became their own apps
     * on the home screen. */
    static const char *NAME[4] = { "Bricks", "Pinball", "Marble", "Pop" };
    static const uint32_t COL[4] = { 0x2E6E5A, 0x6E2E4A, 0x6E5A2E, 0x4A3A6E };

    lv_obj_t *t = lv_label_create(s_root);
    lv_label_set_text(t, "Games");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    /* 🚨 Going to four buttons moved the first one up to -135 (y 63..133).
     * The title at -158 (y 75) ended up behind it and vanished completely,
     * which only showed up in a simulator screenshot.
     * -196 is y 37, where the circle is 252 px wide, so nothing is clipped. */
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -196);

    /* 🚨 Four now. On a round screen y=±172 leaves 314 px of usable width, so
     * the buttons have to come down to 260 or their corners get cut. */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(s_root);
        lv_obj_set_size(b, 260, 70);
        lv_obj_set_style_radius(b, 35, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL[i]), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        /* Three rows of two. The middle row is widest, so it spreads a little further out. */
        lv_obj_align(b, LV_ALIGN_CENTER, 0, -135 + i * 90);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, NAME[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_GAME);
    s_root = lv_obj_create(root);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    show_menu();
}

static void leave(void)
{
    s_defer_fn = NULL;      /* a pending switch would land on a deleted board */
    launcher_handle_show(true);
    stop_loop();
    water_stop();
    orb_stop();
    port_tone_enable(false);
    port_tone_hold(false);   /* let the codec go on the way out */
    s_root = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x7FB0FF); }

/* ── doors straight from home ────────────────────────────────
 * Water and the planets open from the home screen without going through the
 * games menu. They use the same board — two copies of this would mean fixing
 * one of them and not the other. */
static void enter_water(lv_obj_t *root)
{
    enter(root);              /* build the board (this draws the menu) */
    lv_obj_clean(s_root);     /* then take the menu away */
    launcher_handle_show(true);
    s_loop = water_start(s_root);
}

/* ── the planets ─────────────────────────────────────────────
 * Pick one of five. Five separate apps would crowd the home screen, and there
 * is only one renderer and one table anyway. The choice is remembered and
 * opens straight into it next time. */
static orb_kind_t s_orb_pick = ORB_MOON;

static void orb_go(lv_event_t *e)
{
    s_orb_pick = (orb_kind_t)(intptr_t)lv_event_get_user_data(e);
    defer(do_orb);
}

static void do_orb(void)
{
    lv_obj_clean(s_root);
    launcher_handle_show(true);
    s_loop = orb_start(s_root, s_orb_pick);
    /* 🚨 There was somewhere to go back to and no door to it. Same place and
     * same shape as in the games, but back here means **the planet list**. */
    add_back_to(do_orb_back);
}

static void orb_menu(void)
{
    lv_obj_t *t = lv_label_create(s_root);
    lv_label_set_text(t, "Orbit");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -172);

    /* Each one in its own colour, so you know which is which without reading. */
    static const uint32_t COL[ORB_N] = {
        0x4A4A52,   /* Moon    grey   */
        0x2A5A7E,   /* Earth   blue   */
        0x8A5A18,   /* Sun     orange */
        0x7A5A3A,   /* Jupiter brown  */
    };
    for (int i = 0; i < ORB_N; i++) {
        lv_obj_t *b = lv_button_create(s_root);
        lv_obj_set_size(b, 250, 58);
        lv_obj_set_style_radius(b, 29, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL[i]), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_align(b, LV_ALIGN_CENTER, 0, (i - (ORB_N - 1) * 0.5f) * 66);
        lv_obj_add_event_cb(b, orb_go, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, ORB_NAME[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
}

static void enter_orb(lv_obj_t *root)
{
    enter(root);
    lv_obj_clean(s_root);
    launcher_handle_show(true);
    orb_menu();
}

static lv_color_t tint_water(void) { return lv_color_hex(0x3E9BD8); }
static lv_color_t tint_orb(void)   { return lv_color_hex(0x6A7AA8); }

const badge_app_t app_water = {
    .name = "Water", .art = &app_icon_water, .icon = LV_SYMBOL_TINT, .tint = tint_water,
    .radio = RADIO_OFF, .enter = enter_water, .leave = leave,
};
const badge_app_t app_orb = {
    .name = "Orbit", .art = &app_icon_moon, .icon = LV_SYMBOL_EYE_OPEN, .tint = tint_orb,
    .radio = RADIO_OFF, .enter = enter_orb, .leave = leave,
};

const badge_app_t app_games = {
    .name = "Games", .art = &app_icon_games, .icon = LV_SYMBOL_PLAY, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
