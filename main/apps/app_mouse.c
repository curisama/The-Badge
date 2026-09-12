/* Air mouse. The reason this badge exists at all.
 *
 * The original problem was phone RDP: constantly toggling between touch mode
 * and mouse mode. Connect over BLE HID and the phone sees a real mouse, so
 * the toggle stops existing.
 *
 * Two zones on the trackpad screen:
 *   the inner disc  — drag it, the cursor moves (relative)
 *   the outer ring  — turn it, the page scrolls
 * Clicks come from touch. The only physical button is PWR, which goes through
 * the AXP2101 and cannot be pressed quickly. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define CX          233
#define CY          233
#define PAD_R       150         /* trackpad ends here */
#define RING_R_IN   152         /* scroll ring starts here */
#define MOVE_SLOP   3           /* moving this far counts as a drag */
#define HOLD_MS     500         /* holding this long is a right click */
#define WHEEL_DEG   18          /* one notch per this much rotation */
#define TAPDRAG_MS  320         /* press again within this and it is a drag */
#define TAPDRAG_PX  45          /* and the finger has to land within this */

enum { MODE_NONE, MODE_PAD, MODE_RING, MODE_TWO };

#define TWO_SCROLL_PX  14       /* one notch per this much two-finger drag */

static lv_obj_t   *s_pad, *s_dot, *s_state, *s_hint, *s_ring;
static lv_timer_t *s_hold, *s_release, *s_poll;

static lv_point_t s_last;
static int        s_mode;
static bool       s_moved;
static float      s_ring_acc;
static uint32_t   s_prev_ms;
static float      s_last_ang;

/* Tap-drag: tap, then press again immediately and move, and the button stays
 * down while you move. Without it you cannot select text or drag a window
 * closed over RDP, which is most of what this is for. */
static uint32_t   s_last_release_ms;
static lv_point_t s_last_release_pt;
static bool       s_drag_lock;
static bool       s_two;        /* have two fingers been seen in this touch? */
static int        s_two_acc;

/* ── clicks: press and release go out as two reports ─────────── */

static void release_cb(lv_timer_t *t)
{
    (void)t;
    port_hid_mouse(0, 0, 0, 0);
    s_release = NULL;
}

static void click(unsigned button)
{
    port_hid_mouse(0, 0, button, 0);
    if (s_release) lv_timer_delete(s_release);
    s_release = lv_timer_create(release_cb, 30, NULL);
    lv_timer_set_repeat_count(s_release, 1);
}

/* ── acceleration curve ──────────────────────────────────────
 * Stepped gains (1x / 1.5x / 2.5x / 3.5x) make the cursor jump every time the
 * speed crosses a step. This raises the multiplier continuously instead:
 * starts at 1.0, stops at 3.5.
 *
 * Multiplying leaves a fraction, and throwing it away means a slow push loses
 * a little on every report and the cursor falls behind. It is carried into
 * the next one. */

/* ── tilt compensation ───────────────────────────────────────
 * Meant to make "push up" go up however the badge is held, even upside down.
 * The angle follows slowly and freezes the moment a finger lands — a
 * reference that rotates mid-drag bends the cursor's path. */
static float s_tilt_deg;        /* how far the device is rotated now */
static float s_tilt_lock;       /* the angle this touch will use */
static bool  s_tilt_valid;


/* Tilt compensation is gone. tilt_poll was never attached to any timer, so
 * s_tilt_valid stayed false forever — it was dead code that never ran once.
 * The badge is worn on a strap anyway, so its orientation does not change.
 * Left here as an identity function. */
static void rotate_delta(int dx, int dy, int *rx, int *ry)
{
    if (!s_tilt_valid) { *rx = dx; *ry = dy; return; }
    float r = s_tilt_lock * 0.0174533f;
    float c = cosf(r), s = sinf(r);
    *rx = (int)lroundf(dx * c - dy * s);
    *ry = (int)lroundf(dx * s + dy * c);
}

/* ── why it felt stiff ────────────────────────────────────────
 * The CST9217 reports 0..465 (boot log: "Resolution X: 466"). 466 steps
 * across 1.75 inches is about 266 DPI — a quarter to a sixth of a laptop
 * trackpad (1000-1600). The pad is also only 44 mm across, so crossing a
 * screen needs a large gain, and gain multiplies that coarse grid along with
 * everything else. That is what the steppiness was.
 *
 * But there was a bigger culprit. It used to send one report per touch event.
 * Touch runs at 83 Hz (12 ms); the BLE connection interval is whatever the
 * phone grants, usually 66 Hz (15 ms). They do not divide, so some reports
 * carried one touch step and some carried two. That beat is what you felt.
 *
 * So "send on every event" is gone and speed is the thing passed around:
 *   a touch arrives  -> update the instantaneous speed (counts per second)
 *   the timer fires  -> emit an amount proportional to elapsed time
 * That makes it independent of how many events arrived, and the beat goes
 * away. Fractions are carried, so no distance is lost either.
 *
 * The numbers came from imitating slow, normal and fast drags on a PC. Jitter
 * at normal speed is half what it was, and a fast flick travels 1.3x further. */

#define GAIN_MIN   1.00f    /* very slow. Below 1 it is precise but starts skipping */
#define GAIN_DIV   70.0f    /* grows with the square of speed (55 -> 70 to slow it slightly) */
#define GAIN_CAP   3.8f     /* was 4.5 */
#define VEL_SMOOTH 0.35f    /* inertia in the speed estimate; higher is snappier, lower smoother */
#define VEL_IDLE_MS   30    /* no input for this long means stopped, and it decays */
#define VEL_IDLE_DECAY 0.4f

static float      s_vel_x, s_vel_y;      /* counts per second */
static float      s_frac_x, s_frac_y;    /* fractions not yet sent */
static uint32_t   s_last_in_ms;
static lv_timer_t *s_emit;
static int        s_emit_ms = 15;

/* ── air mouse ───────────────────────────────────────────────
 * Tilt the badge and the cursor moves. You are not dragging a finger, you are
 * pointing it like a laser pointer — the angle you tilt becomes the cursor's
 * speed (rate control). There is no way to know an absolute position in the
 * air, so this is the only thing that works.
 *
 * Whatever attitude it is in when you switch it on is the centre, so lying
 * down or arm outstretched both work. Taps still click while it is on —
 * pointing is not much use if you cannot press anything. */
static bool      s_air;
static bool      s_air_default;   /* did we come in as the air mouse from home? */
static lv_obj_t *s_air_btn, *s_air_lbl;
/* The screen used in air mode: two buttons, left and right click, and nothing
 * else. The hand that points is the hand that presses, so where you press
 * must not shake the cursor — these do not affect it at all.
 *
 * 🚨 There used to be a dedicated vertical scroll strip down the middle. It
 * is gone: splitting the screen in three left too little room to drag on, and
 * the strip overlapped the air/trackpad toggle above it by 9 px. The buttons
 * are the scroll surface now — tap to click, drag up and down to scroll.
 * s_air_bg is a transparent sheet underneath so the edges the buttons do not
 * cover can be dragged too. */
static lv_obj_t *s_air_l, *s_air_r, *s_air_bg, *s_disc;
static int32_t   s_scroll_y;
static float     s_scroll_acc;
static float     s_air0x, s_air0y;      /* attitude at switch-on */
static bool      s_air0_set;
/* ── matching whatever pointer speed the host uses ─────────────
 * 🚨 A mouse does not say "move this many pixels". It says **how many counts
 * it moved**. Turning counts into pixels is the host's job, and the ratio
 * differs per machine (pointer speed on Windows, tracking speed on macOS).
 * A sensitivity dialled in on one PC has no reason to be right on another —
 * this is not a bug to fix but a thing to **make adjustable**, for the same
 * reason gaming mice put a DPI button on the mouse.
 *
 * 🚨 This applies to **the trackpad as well**, not just the air mouse. The
 * host's counts-to-pixels ratio hits both equally, so changing only one
 * unbalances them. It is applied once, just before sending.
 *
 * 🚨 Remembered per device. If the PC at home and the one at work run
 * different pointer speeds, having to redial it on every move means nobody
 * uses it. */
#define SENS_N 5
static const float SENS[SENS_N] = { 0.55f, 0.75f, 1.0f, 1.35f, 1.8f };
#define SENS_DEF 2
static uint8_t   s_sens = SENS_DEF;
static lv_obj_t *s_sens_btn, *s_sens_lbl;

/* The per-device table in NVS. Eight entries — bonding holds fifteen, but
 * nobody uses this as a mouse with more machines than that. When it fills,
 * the oldest slot is reused. */
#define SENS_SLOTS 8
typedef struct { uint8_t addr[6]; uint8_t lv; uint8_t used; } sens_row_t;

static void sens_load(void)
{
    uint8_t a[6];
    s_sens = SENS_DEF;
    if (!port_hid_peer_addr(a)) return;        /* not connected yet — defaults */
    sens_row_t t[SENS_SLOTS];
    if (!port_kv_read("mousesens", t, sizeof t)) return;
    for (int i = 0; i < SENS_SLOTS; i++)
        if (t[i].used && memcmp(t[i].addr, a, 6) == 0 && t[i].lv < SENS_N) {
            s_sens = t[i].lv;
            return;
        }
}

static void sens_save(void)
{
    uint8_t a[6];
    if (!port_hid_peer_addr(a)) return;        /* do not record it if we do not know whose it is */
    sens_row_t t[SENS_SLOTS];
    if (!port_kv_read("mousesens", t, sizeof t)) memset(t, 0, sizeof t);
    int slot = -1;
    for (int i = 0; i < SENS_SLOTS; i++)
        if (t[i].used && memcmp(t[i].addr, a, 6) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < SENS_SLOTS; i++) if (!t[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;                    /* full — push out the front */
    memcpy(t[slot].addr, a, 6);
    t[slot].lv = s_sens;
    t[slot].used = 1;
    port_kv_write("mousesens", t, sizeof t);
}

static void sens_paint(void)
{
    if (s_sens_lbl) lv_label_set_text_fmt(s_sens_lbl, "%d", s_sens + 1);
}

static void sens_cb(lv_event_t *e)
{
    (void)e;
    s_sens = (uint8_t)((s_sens + 1) % SENS_N);
    sens_paint();
    sens_save();
    if (s_hint) lv_label_set_text_fmt(s_hint, "speed %d/%d", s_sens + 1, SENS_N);
}

static bool      s_cal_msg;             /* have we said we are zeroing? */
static bool      s_hgrab, s_harmed;     /* started on the handle / pulled it home */
static int32_t   s_hy0;
static float     s_gb_x, s_gb_y, s_gb_z; /* learned gyro bias (dps) */
/* Which way is down, in the sensor's frame. Filtered hard — see below. */
static float     s_dn_x, s_dn_y, s_dn_z;
static bool      s_dn_set;
/* The horizontal axis: perpendicular to gravity and closest to screen-right.
 * Hold the badge on its side and it cannot be worked out for a moment, so the
 * previous one is kept. */
static float     s_ha_x, s_ha_y, s_ha_z;
static bool      s_ha_set;
static uint16_t  s_gb_n;                /* samples taken while zeroing */
static uint32_t  s_gb_t0;
#define AIR_DEAD   24.0f     /* below this it does not move (hand shake) */
/* 0.055 -> 0.078 -> 0.160 -> 0.300 -> 0.500. Every one of those was still too
 * slow on the actual board. Crossing the screen took a big wrist movement,
 * and "the pointer is struggling to keep up with my hand". The response is
 * squared, so doubling the gain doubles the speed at every angle — and the
 * cap has to come up with it, or a large tilt just clips there and feels
 * stuck. */
#define AIR_GAIN   0.500f    /* tilt -> speed, horizontal */
/* 🚨 Vertical lagged behind horizontal ("left-right is fine, up-down needs to
 * be faster"). A wrist rolls side to side through a wider angle than it
 * pitches forward and back, so the same gain moves the cursor less
 * vertically. Each axis gets its own. */
#define AIR_GAIN_Y 0.850f    /* tilt -> speed, vertical */
#define AIR_MAX    5000.0f   /* pixels per second, capped */

/* ── gyro ─────────────────────────────────────────────────────
 * 🚨 Tilt measures *attitude*. Carry the badge across the room without
 * changing how it is held and the cursor does not move — I knew to tilt it,
 * but handing it to someone who waves their arm produces nothing at all.
 * A gyro measures *how far it turned*, so the angle your wrist sweeps becomes
 * the distance the cursor travels. This is what a TV remote air mouse does.
 *
 * With no gyro, or before it can be trusted, this falls back to tilt — see
 * air_drive below. */
/* 34 was slightly fast on the board. Only this side came down; the trackpad
 * was left alone. */
#define GYRO_PX_DEG  26.0f   /* pixels per degree turned */
#define GYRO_CURVE   220.0f  /* gain doubles at this speed (dps) */
#define GYRO_CURVE_CAP 2.4f  /* and no further than this */
/* 🚨 A gyro does not read zero at rest — it has a bias. Leave it in and the
 * cursor drifts on its own.
 *
 * 🚨 It really did drift on the first attempt ("it keeps sliding right when
 * I hold still"). The cause was that the learning condition locked itself
 * out. One early sample was taken as the bias, and being taken right after
 * power-on it was several dps off; the threshold "only learn when quieter
 * than 3 dps" was then already exceeded by that very error, so the learning
 * code never ran again. Threshold-gated learning dies exactly like this.
 *
 * Three things fix it:
 *   1. For GYRO_CAL_MS after switching on, the cursor does not move and an
 *      average is taken. An average of dozens of samples does not hinge on
 *      one bad one.
 *   2. After that it can never lock out. Very quiet learns fast, middling
 *      learns very slowly, moving does not learn — three bands mean there is
 *      always a way back.
 *   3. Slow aiming (under 8 dps) must not be eaten as bias, so that band
 *      learns at a crawl. At 34 px/degree, 8 dps is 272 px/s, which is a
 *      speed people actually use. */
#define GYRO_CAL_MS  700     /* measure only, for this long after switch-on */
#define GYRO_DEAD    1.5f    /* dps of hand shake left after removing the bias */
#define GYRO_QUIET   2.0f    /* quieter than this: learn fast */
#define GYRO_STILL   8.0f    /* quieter than this: learn very slowly */
/* 🚨 The board logged a y-axis bias of -6 to -7.4 dps. At 34 px/degree that
 * is 200 px/s, which is plainly visible. It did learn, but **far too slowly**
 * — held in a hand the residual swings 10-17 dps, which almost never falls
 * into the fast band above. So for a while after zeroing, open the threshold
 * wide and catch the large errors first. */
#define GYRO_WIDE_MS 2500    /* wide threshold for this long */
#define GYRO_WIDE    25.0f   /* and this is that threshold */
/* 🚨 A very slow leak that runs in every state (time constant 25 s). Set it
 * to zero and the estimate can be stranded outside every threshold with no
 * way back — which is what happened the first time. */
#define GYRO_LEAK    0.0006f
#define GYRO_MAXDPS  600.0f
/* 🚨 Reusing the tilt cap (AIR_MAX 5000) clips at 120 dps, which is about one
 * flick of the wrist (confirmed in the simulator). "It goes as far as you
 * turn it" is the whole point of the gyro, and clipping there breaks that
 * relationship. This one is separate and higher.
 * The real ceiling is not here but in the HID report — see emit_cb. */
#define GYRO_MAXPX   16000.0f

static float accel_gain(float a)
{
    float g = GAIN_MIN + a * a / GAIN_DIV;
    return g > GAIN_CAP ? GAIN_CAP : g;
}

/* One touch step arrived. Store it as a speed, not as a distance. */
static void push_move(float dx, float dy, float dt)
{
    float a = sqrtf(dx * dx + dy * dy);
    float g = accel_gain(a);
    s_vel_x += VEL_SMOOTH * ((dx * g) / dt - s_vel_x);
    s_vel_y += VEL_SMOOTH * ((dy * g) / dt - s_vel_y);
    s_last_in_ms = lv_tick_get();
}

#define IDLE_RELEASE_MS 180000   /* hands off for three minutes and the screen may sleep */

/* Turn the wheel by how far you drag.
 * 🚨 The direction follows a mouse — drag up and the page goes up. That is
 * the opposite of the touch feel of dragging paper around, but this is a mouse. */
static void scroll_feed(int32_t y)
{
    s_scroll_acc += (float)(y - s_scroll_y) * 0.06f;
    s_scroll_y = y;
    while (s_scroll_acc >=  1.0f) { s_scroll_acc -= 1.0f; port_hid_mouse(0, 0, 0,  1); }
    while (s_scroll_acc <= -1.0f) { s_scroll_acc += 1.0f; port_hid_mouse(0, 0, 0, -1); }
}

/* One finger does two jobs: tap to click, drag up or down to scroll.
 *
 * 🚨 Pressing the button down on contact makes every drag a click too. So
 * pressing down is deferred, and there are three ways it resolves:
 *   moved more than GES_MIN vertically  -> scroll; the button never goes down
 *   still there after GES_HOLD_MS       -> press (for holding while tilting)
 *   lifted before either               -> press and release on the spot (a quick click)
 * Once decided it stays decided until that finger lifts, so a shaky hand
 * mid-drag does not turn into a scroll. */
#define GES_MIN     14
#define GES_HOLD_MS 150
enum { GES_NONE, GES_HOLD, GES_SCROLL };
static uint8_t  s_ges;
static int32_t  s_ges_y0;
static uint32_t s_ges_t0;
static unsigned s_ges_btn;

static void click_btn_cb(lv_event_t *e)
{
    unsigned btn = (unsigned)(intptr_t)lv_event_get_user_data(e) ? 2 : 1;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);

    if (code == LV_EVENT_PRESSED) {
        s_ges = GES_NONE;
        s_ges_y0 = p.y;
        s_ges_t0 = lv_tick_get();
        s_ges_btn = btn;
        s_scroll_y = p.y;
        s_scroll_acc = 0;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (s_ges == GES_NONE) {
            int32_t d = p.y - s_ges_y0;
            if (d > GES_MIN || d < -GES_MIN) {
                s_ges = GES_SCROLL;
                s_scroll_y = p.y;         /* drop the travel used up reaching the threshold, or the first notch jumps */
                s_scroll_acc = 0;
            } else if (lv_tick_get() - s_ges_t0 > GES_HOLD_MS) {
                s_ges = GES_HOLD;
                port_hid_mouse(0, 0, s_ges_btn, 0);
            }
        }
        if (s_ges == GES_SCROLL) scroll_feed(p.y);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_ges == GES_HOLD) {
            port_hid_mouse(0, 0, 0, 0);
        } else if (s_ges == GES_NONE && code == LV_EVENT_RELEASED) {
            /* Neither threshold nor time reached = a tap. A lost press
             * (PRESS_LOST) means the finger wandered off, which is not a click. */
            port_hid_mouse(0, 0, s_ges_btn, 0);
            port_hid_mouse(0, 0, 0, 0);
        }
        s_ges = GES_NONE;
    }
}

/* The edges the buttons do not cover. Drag only — no clicks happen here. */
static void air_bg_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_scroll_y = p.y; s_scroll_acc = 0; return;
    }
    scroll_feed(p.y);
}

/* 🚨 The air mouse does not aim with the screen — you tilt your wrist and the
 * cursor moves. So the screen should be entirely buttons, laid out like a
 * real mouse seen from above: left half is left click, right half is right.
 * That is all of it.
 *
 * 🚨 At 232 px tall they stopped around the middle of the screen ("the
 * buttons only cover the middle"). Only two things need room above and below:
 *   above, y<90    the air/trackpad toggle (y 48..82). The scroll strip used
 *                  to overlap this.
 *   below, y>398   the handle home (the launcher puts it at y 402..466).
 * Everything between is used: y 90..396, 306 tall. The corner radius is a
 * generous 56 so the corners are cut back before the round screen cuts them. */
static lv_obj_t *mk_click_btn(lv_obj_t *root, int dx, int dy, const char *txt, int right)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, 226, 306);
    lv_obj_set_style_radius(b, 56, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1D1D24), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    /* 🚨 PRESSING is what makes dragging visible. Without it every drag fell
     * through as a click (caught in the simulator — the wheel never moved a
     * notch). PRESS_LOST is handled too: if the finger leaves the button while
     * down, RELEASED never arrives, and not releasing then leaves the button
     * held down on the host. */
    lv_obj_add_event_cb(b, click_btn_cb, LV_EVENT_PRESSED,    (void *)(intptr_t)right);
    lv_obj_add_event_cb(b, click_btn_cb, LV_EVENT_PRESSING,   (void *)(intptr_t)right);
    lv_obj_add_event_cb(b, click_btn_cb, LV_EVENT_RELEASED,   (void *)(intptr_t)right);
    lv_obj_add_event_cb(b, click_btn_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)right);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x6E7686), 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    return b;
}

static void air_paint(void)
{
    if (!s_air_btn) return;
    lv_obj_set_style_bg_color(s_air_btn, lv_color_hex(s_air ? 0x2E6E5A : 0x24242A), 0);
    if (s_air_lbl)
        lv_obj_set_style_text_color(s_air_lbl, lv_color_hex(s_air ? 0xFFFFFF : 0x8A8A90), 0);
    if (s_hint) lv_label_set_text(s_hint, s_air ? "aim to move" : "tap twice, then drag");
    /* In air mode, hide the trackpad and show the buttons; and the reverse. */
    #define SHOW(o, on) do { if (o) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); \
                                      else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } } while (0)
    /* 🔋 The gyro only spins in air mode, and stops the moment you go back to
     * the trackpad — it costs more than ten times the accelerometer. */
    port_imu_gyro_enable(s_air);
    SHOW(s_air_l, s_air);
    SHOW(s_air_r, s_air);
    SHOW(s_air_bg, s_air);
    SHOW(s_ring, !s_air);
    SHOW(s_pad, !s_air);
    SHOW(s_disc, !s_air);      /* the trackpad disc is hidden in air mode */
    #undef SHOW
    /* 🚨 In air mode the middle of the screen is one big button, so the status
     * text ends up behind it. It moves below the buttons so the pairing code
     * is not covered.
     * 🚨 The buttons cover nearly everything, so there is nowhere to put the
     * text beside them. It goes on top: a label is not CLICKABLE and does not
     * take touches, so pressing where it sits still clicks (it is brought to
     * the front below). */
    if (s_state) lv_obj_align(s_state, LV_ALIGN_CENTER, 0, s_air ? -100 : -18);
    if (s_hint)  lv_obj_align(s_hint,  LV_ALIGN_CENTER, 0, s_air ?  -74 : 18);
    /* 🚨 The trackpad gets the handle too, but with its grab area removed —
     * the whole screen is input there and a drag that starts low must not
     * die. pad_cb sorts it out itself:
     *   started on the handle and pulled up  -> home
     *   started on the handle and turned     -> just a scroll, not home
     *   started anywhere else                -> as usual */
    launcher_handle_show(true);
    launcher_handle_passthrough(!s_air);
}

static void air_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        /* For when you are trying to pair a different machine and the phone keeps grabbing it first */
        int n = port_hid_forget_all();
        if (s_hint) lv_label_set_text_fmt(s_hint, "forgot %d device%s", n, n == 1 ? "" : "s");
        return;
    }
    s_air = !s_air;
    s_air0_set = false;      /* take a fresh attitude reference */
    s_gb_n = 0;              /* and re-zero the gyro right here */
    s_gb_x = s_gb_y = s_gb_z = 0;
    s_dn_set = s_ha_set = false;
    s_vel_x = s_vel_y = 0;   /* so leftover speed does not jump */
    air_paint();
}

/* 🚨 Judged per axis. It used to gate on max(|ax|,|ay|), which means a noisy
 * axis stops the quiet one from learning — the logs showed x sitting still
 * while y wobbled, and neither one moving. */
static float learn_k(float a, bool wide)
{
    float m = fabsf(a);
    if (wide && m < GYRO_WIDE) return 0.04f;   /* just after switch-on: big errors first */
    if (m < GYRO_QUIET)        return 0.05f;   /* definitely still */
    if (m < GYRO_STILL)        return 0.004f;  /* ambiguous — crawl, so aiming is not eaten */
    return GYRO_LEAK;                          /* even while moving, a trickle */
}

/* Angular rate straight to cursor speed: it goes as far as you turn it. */
/* 🚨 Only the gyro's x and y were used, which meant **sweeping your arm
 * sideways moved nothing** ("you have to tilt your wrist for it to work").
 *
 * Hold the badge flat, like a tray:
 *   swing your arm up and down = rotation about an axis in the screen plane
 *                                -> shows up in x and y -> this worked
 *   swing it left and right    = **yaw about the gravity axis**
 *                                -> shows up only in z -> this was ignored
 *   tilt your wrist            = an axis in the screen plane -> x, y
 *                                -> which is why only this worked
 *
 * 🚨 Adding z to the horizontal is not the fix. Stand the badge up and z is
 * no longer the gravity axis, so it breaks the other way. **The axes have to
 * be rebuilt around gravity**, and the accelerometer says which way is down:
 *     yaw (horizontal)   = angular rate projected onto the gravity vector
 *     pitch (vertical)   = projected onto the axis perpendicular to gravity
 *                          and closest to screen-right
 * Then, however you hold it — flat, upright, rolled over — swinging your arm
 * sideways moves the cursor sideways. This is what a real air mouse does.
 *
 * 🚨 The trade is that **rolling your wrist no longer moves it sideways**.
 * Roll is rotation about the direction you are pointing, so it does not point
 * anywhere new — and that is the correct behaviour. */
static void air_drive_tilt(void);   /* fallback when the gyro cannot be trusted */

static void air_drive_gyro(float rx, float ry, float rz)
{
    /* 1) Right after switch-on, only measure. The cursor does not move during
     * this — it is a 0.4 s zeroing, so switching on while holding it makes
     * that attitude the zero. */
    if (s_gb_n == 0) s_gb_t0 = lv_tick_get();
    if (lv_tick_get() - s_gb_t0 < GYRO_CAL_MS) {
        s_gb_x += (rx - s_gb_x) / (float)(s_gb_n + 1);
        s_gb_y += (ry - s_gb_y) / (float)(s_gb_n + 1);
        s_gb_z += (rz - s_gb_z) / (float)(s_gb_n + 1);
        if (s_gb_n < 60000) s_gb_n++;
        s_vel_x = s_vel_y = 0;
        /* Say that a motionless cursor is not a fault */
        if (s_hint && !s_cal_msg) { lv_label_set_text(s_hint, "hold still"); s_cal_msg = true; }
        return;
    }
    if (s_cal_msg) {
        s_cal_msg = false;
        if (s_hint) lv_label_set_text(s_hint, "aim to move");
    }

    float wx = rx - s_gb_x;
    float wy = ry - s_gb_y;
    float wz = rz - s_gb_z;

    /* 2) After that it keeps tracking in three bands. 🚨 The point is that no
     * band can strand it in a state where it cannot learn — stranded means the
     * cursor drifts forever.
     * 🚨 It does not learn while moving. Learning then would record that
     * movement as zero, and the cursor would snap backwards when you stop.
     * 🚨 z has to be learned too: the horizontal now rides on z, so leaving
     * its bias in makes the cursor slide sideways on its own. */
    bool wide = (lv_tick_get() - s_gb_t0) < (GYRO_CAL_MS + GYRO_WIDE_MS);
    s_gb_x += wx * learn_k(wx, wide);
    s_gb_y += wy * learn_k(wy, wide);
    s_gb_z += wz * learn_k(wz, wide);

    /* 3) Which way is down, from the accelerometer.
     * 🚨 Shake it and the accelerometer reads your arm as well as gravity, so
     * this is filtered hard (time constant about 0.7 s). Attitude changes
     * slowly, so that is still fast enough. */
    float mx = 0, my = 0, mz = 0;
    if (port_imu_accel3(&mx, &my, &mz)) {
        /* The accelerometer reads positive on the axis pointing at the sky, so down is the negation */
        if (!s_dn_set) { s_dn_x = -mx; s_dn_y = -my; s_dn_z = -mz; s_dn_set = true; }
        else {
            s_dn_x += (-mx - s_dn_x) * 0.03f;
            s_dn_y += (-my - s_dn_y) * 0.03f;
            s_dn_z += (-mz - s_dn_z) * 0.03f;
        }
    }
    float dl = sqrtf(s_dn_x * s_dn_x + s_dn_y * s_dn_y + s_dn_z * s_dn_z);
    if (dl < 200.0f) { air_drive_tilt(); return; }   /* not trustworthy yet */
    float gxu = s_dn_x / dl, gyu = s_dn_y / dl, gzu = s_dn_z / dl;

    /* 4) The horizontal axis: screen-right, flattened into the horizontal plane.
     * In the sensor's frame screen-right is (0,-1,0) — the same axes measured
     * for the marble and the water.
     * 🚨 Stand the badge on its side, right edge down, and this collapses to
     * nothing. Then the previous axis is kept: the direction must not flip. */
    float rxv = 0.0f, ryv = -1.0f, rzv = 0.0f;
    float rdotg = rxv * gxu + ryv * gyu + rzv * gzu;
    float hx = rxv - rdotg * gxu, hy = ryv - rdotg * gyu, hz = rzv - rdotg * gzu;
    float hl = sqrtf(hx * hx + hy * hy + hz * hz);
    if (hl > 0.30f) {
        s_ha_x = hx / hl; s_ha_y = hy / hl; s_ha_z = hz / hl;
        s_ha_set = true;
    }
    if (!s_ha_set) { air_drive_tilt(); return; }

    /* 5) Project the angular rate onto those two axes. */
    float yawr   = wx * gxu + wy * gyu + wz * gzu;          /* swinging sideways */
    float pitchr = wx * s_ha_x + wy * s_ha_y + wz * s_ha_z; /* lifting up and down */

    /* One line every two seconds, for when drift is not visible by eye. */
    static uint32_t log_ms;
    if (lv_tick_get() - log_ms > 2000) {
        log_ms = lv_tick_get();
        port_log("air", "bias %.2f/%.2f/%.2f  yaw %.1f pitch %.1f  down %.2f/%.2f/%.2f",
                 (double)s_gb_x, (double)s_gb_y, (double)s_gb_z,
                 (double)yawr, (double)pitchr,
                 (double)gxu, (double)gyu, (double)gzu);
    }

    /* 🚨 Dead zone and curve go on the **projected** values. Applied to the
     * raw axes, one movement splits across two while the badge is held at an
     * angle and neither half clears the threshold. */
    #define DEAD(v) ((v) > GYRO_DEAD ? (v) - GYRO_DEAD : ((v) < -GYRO_DEAD ? (v) + GYRO_DEAD : 0))
    float ax = DEAD(yawr), ay = DEAD(pitchr);
    #undef DEAD
    if (ax >  GYRO_MAXDPS) ax =  GYRO_MAXDPS;
    if (ax < -GYRO_MAXDPS) ax = -GYRO_MAXDPS;
    if (ay >  GYRO_MAXDPS) ay =  GYRO_MAXDPS;
    if (ay < -GYRO_MAXDPS) ay = -GYRO_MAXDPS;

    /* Turn slowly for precision, quickly for distance. Not as aggressively as
     * the tilt side squares it — a gyro is already a rate, and squaring a rate
     * makes it impossible to aim. */
    float sp = sqrtf(ax * ax + ay * ay);
    float g = 1.0f + sp / GYRO_CURVE;
    if (g > GYRO_CURVE_CAP) g = GYRO_CURVE_CAP;
    g *= GYRO_PX_DEG;

    /* 🚨 The vertical sign carries over exactly from the old code: held flat,
     * pitch-up comes out as -gyro_y, which is the expression the vertical has
     * always used. It is a verified value and is not flipped again here.
     * 🚨 The horizontal is a new axis. Swinging your arm to the right turns
     * the badge clockwise seen from above, so the angular rate points along
     * gravity, yaw is positive, and the cursor goes right. Confirmed on the
     * board. */
    float vx =  ax * g;
    float vy =  ay * g;
    if (vx >  GYRO_MAXPX) vx =  GYRO_MAXPX;
    if (vx < -GYRO_MAXPX) vx = -GYRO_MAXPX;
    if (vy >  GYRO_MAXPX) vy =  GYRO_MAXPX;
    if (vy < -GYRO_MAXPX) vy = -GYRO_MAXPX;

    /* Only the staircase is smoothed. A gyro is already a rate, so anything more here is just lag. */
    s_vel_x += (vx - s_vel_x) * 0.9f;
    s_vel_y += (vy - s_vel_y) * 0.9f;

    if (vx != 0 || vy != 0) s_last_in_ms = lv_tick_get();
}

/* Tilt as cursor speed, measured as departure from the attitude at switch-on.
 * This handles boards with no gyro (the simulator included) and the moments
 * before the gyro has woken up. */
static void air_drive_tilt(void)
{
    /* 🚨 Reading the IMU on its own timer was the last source of lag. Even
     * pulled in from 20 ms to 10 ms the value could be 10 ms stale. But this
     * function is called **exactly once per report** (emit_cb), so reading it
     * right here is the correct thing — no staleness at all, and it uses less
     * I2C besides (every 10 ms = 100/s, versus per report = 66/s). */
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) return;
    if (!s_air0_set) { s_air0x = gx; s_air0y = gy; s_air0_set = true; }
    float dx = gx - s_air0x, dy = gy - s_air0y;

    /* 🚨 The IMU axes are rotated 90 degrees from the screen's: gx drives the
     * vertical (forward and back), gy the horizontal (left and right). Same
     * as established with the marble.
     * Tilting right makes gy negative, so the sign is flipped to send the
     * cursor right.
     * 🚨 The vertical used to be inverted — tipping it forward sent the
     * cursor up. The horizontal (-dy) was right and only the vertical was
     * the wrong way round. */
    float ax = -dy, ay = dx;

    /* A hand shakes even when held still. Kill that much. */
    #define DEAD(v) ((v) > AIR_DEAD ? (v) - AIR_DEAD : ((v) < -AIR_DEAD ? (v) + AIR_DEAD : 0))
    ax = DEAD(ax); ay = DEAD(ay);
    #undef DEAD

    /* A small tilt moves slowly and a large one moves fast — squared, which
     * is what lets precise aiming and crossing the screen live in one hand. */
    float vx = ax * fabsf(ax) * AIR_GAIN   * 0.01f;
    float vy = ay * fabsf(ay) * AIR_GAIN_Y * 0.01f;
    if (vx >  AIR_MAX) vx =  AIR_MAX;
    if (vx < -AIR_MAX) vx = -AIR_MAX;
    if (vy >  AIR_MAX) vy =  AIR_MAX;
    if (vy < -AIR_MAX) vy = -AIR_MAX;

    /* Applying it directly carries the shake through. Approach it slightly
     * behind instead.
     * 🚨 At 0.35 the time constant is 43 ms against a 15 ms report period,
     * which is noticeably late. 0.55 was still not enough; 0.88 is about
     * 4 ms. The shake is already handled by the dead zone above, so there is
     * no reason to damp it twice — this smoothing exists only to take the
     * staircase off, not to remove shake. */
    s_vel_x += (vx - s_vel_x) * 0.88f;
    s_vel_y += (vy - s_vel_y) * 0.88f;

    /* 🚨 The air mouse never touches the screen. Left alone that reads as
     * idle, the display sleeps, and reports drop to 500 ms — the cursor
     * stops. Moving by tilt is input too: stay awake only while it is
     * actually moving. Hold it still and it sleeps as usual. */
    if (vx != 0 || vy != 0) s_last_in_ms = lv_tick_get();
}

static void air_drive(void)
{
    float rx = 0, ry = 0, rz = 0;
    if (port_imu_gyro(&rx, &ry, &rz)) { air_drive_gyro(rx, ry, rz); return; }
    air_drive_tilt();
}

static void emit_cb(lv_timer_t *t)
{
    /* 🔋 Nobody is looking at a dark screen. 🚨 But do not change the period
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so calling it from a timer callback restarts the handler on the spot
     * and it never returns (100% CPU, found while trying to save power).
     * Passing the period it already has does the same, so "only set it when
     * it changes" does not save you either.
     * Leave the period and act on every 33rd call. Same effect, and safe. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 33) return;
    }
   /* Back to the value that matches the connection interval */
    /* 🚨 .keep_awake = true used to be set with no limit, so leaving the app
     * open kept the display on forever (on versus off is 124 mV per hour).
     * Hold it only while it is in use and let go when hands come off. Touch
     * it again and it comes straight back. */
    launcher_keep_awake(lv_tick_get() - s_last_in_ms < IDLE_RELEASE_MS);
    float dt = s_emit_ms / 1000.0f;

    if (s_air) air_drive();

    /* If the finger stopped and speed is left over, the cursor slides on.
     * In air mode tilt is still driving it, so skip this. */
    if (!s_air && lv_tick_get() - s_last_in_ms > VEL_IDLE_MS) {
        s_vel_x *= VEL_IDLE_DECAY;
        s_vel_y *= VEL_IDLE_DECAY;
        if (fabsf(s_vel_x) < 1.0f) s_vel_x = 0;
        if (fabsf(s_vel_y) < 1.0f) s_vel_y = 0;
    }

    /* 🚨 Applied once, here. Tilt, gyro and trackpad all pass through this
     * point, so their balance with each other is preserved. */
    float k = SENS[s_sens < SENS_N ? s_sens : SENS_DEF];
    float wx = s_vel_x * dt * k + s_frac_x;
    float wy = s_vel_y * dt * k + s_frac_y;
    int ix = (int)wx, iy = (int)wy;
    /* 🚨 A HID report carries ±127 pixels at most (8-bit relative). Dropping
     * the excess means a fast flick travels less than it should — and "it
     * goes as far as you turn it" is the whole gyro. Clamp, and carry the
     * remainder into the next report. */
    if (ix >  127) ix =  127;
    if (ix < -127) ix = -127;
    if (iy >  127) iy =  127;
    if (iy < -127) iy = -127;
    s_frac_x = wx - (float)ix;      /* unsent fraction and leftover, into the next one */
    s_frac_y = wy - (float)iy;
    /* 🚨 But an unbounded backlog makes the cursor keep sliding after your
     * hand stops. Hold no more than a couple of reports' worth — 400 px was
     * far too generous and slid for three or four reports after a flick. */
    #define FRAC_MAX 150.0f
    if (s_frac_x >  FRAC_MAX) s_frac_x =  FRAC_MAX;
    if (s_frac_x < -FRAC_MAX) s_frac_x = -FRAC_MAX;
    if (s_frac_y >  FRAC_MAX) s_frac_y =  FRAC_MAX;
    if (s_frac_y < -FRAC_MAX) s_frac_y = -FRAC_MAX;
    #undef FRAC_MAX
    if (ix || iy) port_hid_mouse(ix, iy, s_drag_lock ? 1 : 0, 0);
}

static void motion_reset(void)
{
    s_vel_x = s_vel_y = 0;
    s_frac_x = s_frac_y = 0;
    s_last_in_ms = lv_tick_get();
}

/* ── hold for right click ───────────────────────────────────── */

static void hold_cb(lv_timer_t *t)
{
    (void)t;
    s_hold = NULL;
    if (!s_moved) {
    s_moved = true;            /* so releasing does not also send a left click */
        click(2);
        lv_label_set_text(s_hint, "right click");
    }
}

static void cancel_hold(void)
{
    if (s_hold) { lv_timer_delete(s_hold); s_hold = NULL; }
}

/* ── fingers ────────────────────────────────────────────────── */

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int rx = p.x - CX, ry = p.y - CY;
    float r = sqrtf((float)(rx * rx + ry * ry));

    if (code == LV_EVENT_PRESSED) {
        /* Did it start on the handle? Remember that it did, but carry on as
         * normal — if it is not pulled up, it has to behave exactly as usual. */
        s_hgrab = !s_air && launcher_handle_zone(p.x, p.y);
        s_harmed = false;
        s_hy0 = p.y;

        uint32_t now = lv_tick_get();
        s_drag_lock = (s_mode != MODE_RING)
                   && (now - s_last_release_ms < TAPDRAG_MS)
                   && (LV_ABS(p.x - s_last_release_pt.x) < TAPDRAG_PX)
                   && (LV_ABS(p.y - s_last_release_pt.y) < TAPDRAG_PX);

        s_last = p;
        s_moved = false;
        s_ring_acc = 0;
        motion_reset();
        s_prev_ms = lv_tick_get();
    s_tilt_lock = s_tilt_deg;      /* this angle is fixed for this touch */
        s_two = false;
        s_two_acc = 0;
        s_mode = (r >= RING_R_IN) ? MODE_RING : MODE_PAD;
        s_last_ang = atan2f((float)ry, (float)rx) * 57.2958f;
        cancel_hold();
        if (s_drag_lock) {
            /* Start with the button down; it stays down until release. */
            port_hid_mouse(0, 0, 1, 0);
            lv_label_set_text(s_hint, "drag");
            lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x5BD48A), 0);
        } else if (s_mode == MODE_PAD) {
            lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x7FB0FF), 0);
            s_hold = lv_timer_create(hold_cb, HOLD_MS, NULL);
            lv_timer_set_repeat_count(s_hold, 1);
        }
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_dot, p.x - 14, p.y - 14);

    } else if (code == LV_EVENT_PRESSING) {
        if (s_hgrab) s_harmed = launcher_handle_drag(p.y - s_hy0);
        lv_obj_set_pos(s_dot, p.x - 14, p.y - 14);
        int dx = p.x - s_last.x, dy = p.y - s_last.y;

        /* Has a second finger touched down? Once it has, remember it for the
         * rest of this touch — both are not necessarily down on release. */
        if (port_touch_count() >= 2) {
            if (!s_two) { s_two = true; cancel_hold(); }
            s_mode = MODE_TWO;
        }

        if (s_mode == MODE_TWO) {
        /* Two-finger drag scrolls, like a trackpad */
            s_two_acc += dy;
            while (s_two_acc >= TWO_SCROLL_PX)  { s_two_acc -= TWO_SCROLL_PX; port_hid_mouse(0, 0, 0, -1); s_moved = true; }
            while (s_two_acc <= -TWO_SCROLL_PX) { s_two_acc += TWO_SCROLL_PX; port_hid_mouse(0, 0, 0,  1); s_moved = true; }
            if (s_moved) lv_label_set_text(s_hint, "two-finger scroll");
        } else if (s_mode == MODE_RING) {
            float ang = atan2f((float)ry, (float)rx) * 57.2958f;
            float d = ang - s_last_ang;
            while (d > 180)  d -= 360;      /* stop it jumping when it crosses 12 o'clock */
            while (d < -180) d += 360;
            s_last_ang = ang;
            s_ring_acc += d;
            while (s_ring_acc >= WHEEL_DEG)  { s_ring_acc -= WHEEL_DEG; port_hid_mouse(0, 0, 0, -1); }
            while (s_ring_acc <= -WHEEL_DEG) { s_ring_acc += WHEEL_DEG; port_hid_mouse(0, 0, 0,  1); }
            if (fabsf(d) > 0.5f) { s_moved = true; cancel_hold(); }
            lv_label_set_text(s_hint, "scroll");
        } else {
            if (abs(dx) > MOVE_SLOP || abs(dy) > MOVE_SLOP) { s_moved = true; cancel_hold(); }
            /* Filter the coordinates first, to take the grid noise out. The
             * delta is then between two filtered values, which removes the
             * single-pixel jitter. */
            uint32_t now_ms = lv_tick_get();
            float dt = (now_ms > s_prev_ms) ? (now_ms - s_prev_ms) / 1000.0f : 0.012f;
            if (dt < 0.004f) dt = 0.004f;
            if (dt > 0.100f) dt = 0.100f;
            s_prev_ms = now_ms;

            if (dx || dy) {
                int rx2, ry2;
                rotate_delta(dx, dy, &rx2, &ry2);
                push_move((float)rx2, (float)ry2, dt);
                if (!s_drag_lock) lv_label_set_text(s_hint, "move");
            }
        }
        s_last = p;

    } else if (code == LV_EVENT_RELEASED) {
        if (s_hgrab) {
            s_hgrab = false;
            launcher_handle_drop();
            if (s_harmed) {
                /* 🚨 launcher_home() closes this app. Touching screen objects
                 * afterwards walks into things that are gone — clean up and
                 * leave immediately. */
                cancel_hold();
                motion_reset();
                port_hid_mouse(0, 0, 0, 0);
                s_mode = MODE_NONE;
                launcher_home();
                return;
            }
        }
        cancel_hold();
        /* Once the hand is off, kill the speed at once or the cursor slides. */
        motion_reset();
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        s_last_release_ms = lv_tick_get();
        s_last_release_pt = p;

        if (s_drag_lock) {
            port_hid_mouse(0, 0, 0, 0);      /* release the button */
            s_drag_lock = false;
            lv_label_set_text(s_hint, "2 fingers = right click");
            s_mode = MODE_NONE;
            return;
        }
        if (!s_moved && s_two) {
            /* Two-finger tap = right click, as on any trackpad.
             * Hold is kept as well — gripping it with a thumb leaves you
             * without two free fingers. */
            click(2);
            lv_label_set_text(s_hint, "right click");
        } else if (!s_moved && s_mode == MODE_PAD) {
            click(1);
            lv_label_set_text(s_hint, "click");
        }
        s_mode = MODE_NONE;
    }
}

/* ── connection status ──────────────────────────────────────── */

/* 🚨 If you only redraw when a value changes, you have to write it at least
 * once while building the screen. Otherwise LVGL's placeholder label text —
 * literally "Text" — is what stays. Same trap as the battery percentage; here
 * it showed "Text" on the trackpad whenever BLE was down or the status
 * happened to match last time. */
static int      s_prev_conn = -1;
static uint32_t s_prev_key  = 0xFFFFFFFF;
static char     s_prev_peer[24];

static void poll_cb(lv_timer_t *t)
{

    /* 🔋 Nobody is looking at a dark screen. 🚨 But do not change the period
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so calling it from a timer callback restarts the handler on the spot
     * and it never returns (100% CPU, found while trying to save power).
     * Passing the period it already has does the same.
     * Leave the period and act on every 5th call. Same effect, and safe. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 5) return;
    }

    /* LVGL redraws whenever a style is set, even to the value it already had.
     * This was invalidating a 452 px ring 2.5 times a second for nothing —
     * only touch it when something changed. */
    /* 🚨 BLE refuses to start while the boot-time clock sync still holds WiFi,
     * because there is not enough internal RAM for both. That clears by itself
     * in a few seconds, so keep asking rather than leaving the app sitting on
     * "busy" until somebody thinks to back out and come in again. */
    if (!port_hid_ready()) port_hid_start();

    int conn = port_hid_connected() ? 1 : 0;
    uint32_t pk = port_hid_passkey();
    const char *peer = port_hid_peer();
    if (conn == s_prev_conn && pk == s_prev_key &&
        strncmp(peer ? peer : "", s_prev_peer, sizeof s_prev_peer) == 0) return;
    s_prev_conn = conn;
    s_prev_key  = pk;
    snprintf(s_prev_peer, sizeof s_prev_peer, "%s", peer ? peer : "");
    uint32_t key = port_hid_passkey();
    if (key) {
        /* Shown large so you can compare it against the number on the phone */
        lv_label_set_text_fmt(s_state, "%06lu", (unsigned long)key);
        lv_obj_set_style_text_color(s_state, lv_color_hex(0xF0F3F6), 0);
        lv_label_set_text(s_hint, "match this on your phone");
        return;
    }

    bool on = port_hid_connected();
    const char *nm = port_hid_peer();
    lv_label_set_text(s_state, (nm && *nm) ? nm : (on ? "connected" : "advertising"));
    lv_obj_set_style_text_color(s_state, lv_color_hex(on ? 0x5BD48A : 0xE0B33A), 0);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(on ? 0x2E6E4A : 0x4A4030), LV_PART_MAIN);
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_MOUSE);
    /* The outer ring is both the scroll area and the connection indicator */
    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, 452, 452);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);

    /* The trackpad surface */
    s_disc = lv_obj_create(root);
    lv_obj_remove_style_all(s_disc);
    lv_obj_set_size(s_disc, PAD_R * 2, PAD_R * 2);
    lv_obj_center(s_disc);
    lv_obj_set_style_radius(s_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_disc, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(s_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_disc, 1, 0);
    lv_obj_set_style_border_color(s_disc, lv_color_hex(0x26262C), 0);

    s_prev_conn = -1;                     /* force the next update to write */
    s_prev_key  = 0xFFFFFFFF;
    s_prev_peer[0] = '\0';

    /* 🚨 Going into Settings to switch machines mid-use is a long way round.
     * Tapping the label that names the current host opens the list — the
     * place matches the meaning. */
    s_state = lv_label_create(root);
    lv_label_set_text(s_state, "connecting");   /* overwrite LVGL's placeholder */
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x5E5E66), 0);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_20, 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, -18);

    s_hint = lv_label_create(root);
    lv_label_set_text(s_hint, "tap twice, then drag");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5E5E66), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 18);

    /* Air mouse on and off */
    s_air = s_air_default;
    s_air0_set = false;
    s_air_btn = lv_button_create(root);
    lv_obj_set_size(s_air_btn, 64, 34);
    lv_obj_set_style_radius(s_air_btn, 17, 0);
    lv_obj_set_style_shadow_width(s_air_btn, 0, 0);
    /* 🚨 Sits next to the sensitivity button. Both are 64 px, so ∓40 leaves
     * 16 px between them. The furthest corner is (96,-185), 208 px from
     * centre, inside the 233 radius. */
    lv_obj_align(s_air_btn, LV_ALIGN_CENTER, -40, -168);
    lv_obj_add_event_cb(s_air_btn, air_cb, LV_EVENT_CLICKED, NULL);
    /* Hold to forget every paired device */
    lv_obj_add_event_cb(s_air_btn, air_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_air_lbl = lv_label_create(s_air_btn);
    lv_label_set_text(s_air_lbl, LV_SYMBOL_GPS);
    lv_obj_center(s_air_lbl);
    /* Sensitivity — press it a couple of times on a new PC. Remembered per device. */
    s_sens_btn = lv_button_create(root);
    lv_obj_set_size(s_sens_btn, 64, 34);
    lv_obj_set_style_radius(s_sens_btn, 17, 0);
    lv_obj_set_style_shadow_width(s_sens_btn, 0, 0);
    lv_obj_set_style_bg_color(s_sens_btn, lv_color_hex(0x24242A), 0);
    lv_obj_align(s_sens_btn, LV_ALIGN_CENTER, 40, -168);
    lv_obj_add_event_cb(s_sens_btn, sens_cb, LV_EVENT_CLICKED, NULL);
    s_sens_lbl = lv_label_create(s_sens_btn);
    lv_obj_set_style_text_color(s_sens_lbl, lv_color_hex(0x8A8A90), 0);
    lv_obj_center(s_sens_lbl);
    sens_load();          /* fetch the value for whatever is connected */
    sens_paint();

    /* 🚨 Do not call air_paint() here — the click buttons and the scroll
     * sheet do not exist yet and would be left hidden. It is called once
     * below, after everything is built. (Entering the air mouse straight from
     * home showed no buttons at all.) */

    /* ── the air mouse screen ───────────────────────────────
     * Tilt to aim, press either half to click, drag anywhere to scroll.
     *
     * This transparent sheet catches the edges the buttons do not cover: the
     * strip above, the strip below, and the gap between them. It has to sit
     * behind the buttons, and behind the toggle and the handle too — the
     * toggle is brought forward below, and the handle is on another layer
     * entirely. */
    s_air_bg = lv_obj_create(root);
    lv_obj_remove_style_all(s_air_bg);
    lv_obj_set_size(s_air_bg, 466, 466);
    lv_obj_center(s_air_bg);
    lv_obj_add_flag(s_air_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_air_bg, air_bg_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_air_bg, air_bg_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_flag(s_air_bg, LV_OBJ_FLAG_HIDDEN);

    /* The screen split in half, with only 6 px between — a fingertip is not
     * going to be confused about the boundary, since either side is a valid
     * press. Centre y is 243, i.e. dy +10. */
    s_air_l = mk_click_btn(root, -117, 10, "L", 0);
    s_air_r = mk_click_btn(root,  117, 10, "R", 1);

    /* A dot where the finger is, so you can see where you pressed even with the screen covered */
    s_dot = lv_obj_create(root);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 28, 28);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x7FB0FF), 0);
    lv_obj_set_style_bg_opa(s_dot, 70, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    /* The whole screen takes input; the ring is separated out by coordinate. */
    s_pad = lv_obj_create(root);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_size(s_pad, 466, 466);
    lv_obj_center(s_pad);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_RELEASED, NULL);

    /* 🚨 s_pad covers the entire screen and is created late, so left alone it
     * sits on top and the buttons above it never get pressed (the air button
     * did not respond). Creation order is z-order, so bring them forward
     * explicitly. */
    lv_obj_move_foreground(s_air_bg);
    lv_obj_move_foreground(s_air_l);
    lv_obj_move_foreground(s_air_r);
    /* 🚨 The toggle has to be topmost. The click buttons reach the top edge
     * of the screen, and behind them there is no way back from air mode to
     * the trackpad — the buttons were covering the toggle. */
    lv_obj_move_foreground(s_air_btn);
    lv_obj_move_foreground(s_sens_btn);
    /* The text goes on top of the buttons. A label takes no touches, so it does not block presses. */
    lv_obj_move_foreground(s_state);
    lv_obj_move_foreground(s_hint);

    air_paint();          /* everything exists now — show and hide per mode */

    /* Send at whatever connection interval the phone granted. Sending faster
     * only piles up in the stack's queue and adds latency. Usually 7.5-15 ms. */
    int iv = port_hid_interval_ms();
    if (iv < 6) iv = 6;
    s_emit_ms = iv;
    s_emit = lv_timer_create(emit_cb, iv, NULL);
    port_log("mouse", "report period %d ms (connection interval %d ms)", iv, port_hid_interval_ms());

    s_poll = lv_timer_create(poll_cb, 400, NULL);
    poll_cb(NULL);
}

static void leave(void)
{
    cancel_hold();
    s_drag_lock = false;
    if (s_release) { lv_timer_delete(s_release); s_release = NULL; }
    if (s_poll)    { lv_timer_delete(s_poll);    s_poll = NULL; }
    if (s_emit)    { lv_timer_delete(s_emit);    s_emit = NULL; }
    motion_reset();
    port_hid_mouse(0, 0, 0, 0);      /* do not leave with a button held down */
    /* 🔋 🚨 Always stop it on the way out. Left running, the gyro keeps
     * spinning after the app closes and after the display sleeps — the same
     * thing happened with the accelerometer (fixed there by sleeping it after
     * five seconds). */
    port_imu_gyro_enable(false);
    s_sens_btn = s_sens_lbl = NULL;
    s_gb_n = 0;
    s_gb_x = s_gb_y = s_gb_z = 0;
    s_dn_set = s_ha_set = false;
    s_mode = MODE_NONE;
}

static lv_color_t tint(void) { return lv_color_hex(0x7FB0FF); }

const badge_app_t app_mouse = {
    .name = "Trackpad", .art = &app_icon_mouse, .icon = LV_SYMBOL_GPS, .tint = tint,
    /* Held awake on entry; emit_cb lets go after three minutes hands-off. */
    .radio = RADIO_BLE, .keep_awake = true, .enter = enter, .leave = leave,
};

/* The door straight from home into the air mouse. Same app, different starting mode. */
static void enter_air(lv_obj_t *root) { s_air_default = true; enter(root); s_air_default = false; }

const badge_app_t app_air = {
    .name = "Air Mouse", .art = &app_icon_mouse, .icon = LV_SYMBOL_GPS, .tint = tint,
    /* Held awake on entry; emit_cb lets go after three minutes hands-off. */
    .radio = RADIO_BLE, .keep_awake = true, .enter = enter_air, .leave = leave,
};
