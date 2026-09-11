/* 게임 셋. 다마고치(에뮬레이터)를 걷어내고 그 자리에 넣었다.
 * 셋 다 도형 몇 개만 움직이므로 CPU 도 메모리도 거의 안 쓴다.
 *
 *   회전 피하기 — 벽이 조여온다. 원형 화면이 곧 게임판
 *   벽돌깨기   — 링 패들과 원을 따라 깔린 벽돌. 네모 화면으론 못 만드는 모양
 *   구슬 미로  — 기울여서 굴린다. 놀고 있던 IMU 를 쓴다 */
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

/* 벽돌 개수는 clear_board 가 배열을 훑어야 해서 여기서 먼저 정한다. */
#define BRK_ROWS   4            /* 단계마다 3 또는 4 줄을 쓴다 */
/* 🚨 기체가 없으면 한 번 놓칠 때마다 그 판을 처음부터 다시 쌓는다. 5단계를
 * 거의 다 깨놓고 한 번 놓쳐서 통째로 잃으면 다시 할 마음이 안 든다
 * (0911 제보: "다 깼는데 죽었더니 완전 새로 시작해서 열받더라").
 * 셋을 준다 — 두 번은 봐주고 세 번째에 끝난다. */
#define BRK_LIVES  3
#define BRK_MAX    9            /* 한 줄 최대 개수 */
#define BRK_N      (BRK_ROWS * BRK_MAX)

/* 구슬게임 기준 자세. 탭해서 시작한 순간의 기울기를 0으로 잡는다 —
 * 손목에 차고 있든 책상에 눕혀 있든 "지금 이 자세가 수평"이 되어야
 * 어느 자세에서 시작해도 같은 감각으로 굴릴 수 있다. */
static float s_g0x, s_g0y;
static bool  s_g0_set;

static lv_obj_t   *s_root;      /* 게임이 그려지는 판 */
/* 판 위의 것들. clear_board 가 한꺼번에 끊어야 해서 여기 모아둔다. */
static lv_obj_t   *s_paddle, *s_ball;
/* 기울기 모드 — 배지를 기울여 판을 몬다. 같은 버튼으로 껐다 켠다. */
static bool      s_tiltmode;
static lv_obj_t *s_tilt_btn, *s_tilt_lbl;
static float     s_tilt0;        /* 켤 때의 자세를 가운데로 삼는다 */
static bool      s_tilt0_set;
static lv_obj_t   *s_brick[BRK_N];
static bool        s_alive[BRK_N];
static uint8_t     s_hp[BRK_N];      /* 남은 맷집. 2 면 한 번 더 맞아야 깨진다 */
/* 방해물 — 가운데를 도는 구슬 둘.
 * 🚨 호(lv_arc)로 만들지 마라. 각도를 바꿀 때마다 LVGL 이 그 객체의 **네모
 * 전체**를 무효화한다. 반지름 124 짜리 호면 262x262 = 68,000 픽셀을 매 걸음
 * 다시 밀게 되고 그것만으로 프레임이 반토막 난다. 작은 구슬을 옮기면
 * 무효화되는 건 옛 자리와 새 자리 두 조각뿐이다 — 공이랑 똑같다. */
#define OBS_N     2
#define OBS_R     124.0f        /* 도는 반지름 */
#define OBS_D     26            /* 지름(px) */
static lv_obj_t *s_obs[OBS_N];
static float     s_obs_ang, s_obs_dps;

#define PB_WALL     206.0f   /* 공 중심이 여기까지 나간다 */
#define PB_BR       6.5f     /* 공 반지름 */
#define PB_GRAV     0.085f   /* 걸음(20ms)당 아래로 붙는 속도 (기본 경사) */
/* 기울기 1g(=1000) 가 기본 경사보다 조금 더 미는 정도. 세워 들면 둘이 더해져
 * 두 배 남짓 빨라진다 — 세운 만큼 가팔라지는 게 맞다. */
#define PB_TILT     0.00011f
#define PB_DAMP     0.996f
#define PB_WALL_E   0.84f    /* 벽은 조금 먹는다 */
#define PB_MAXV     14.0f
#define PB_BUMP_N   5
#define PB_BUMP_R   21.0f
/* 쓰러뜨리는 표적 — 맞으면 사라지고 다 쓰러뜨리면 한꺼번에 다시 선다.
 * 🚨 범퍼만 있으면 공이 어디로 가든 점수가 같아서 겨눌 이유가 없다.
 * "다음에 뭘 노릴까" 가 생겨야 판이 넓게 쓰인다. */
#define PB_TGT_N    4
#define PB_TGT_R    9.0f
#define PB_BUMP_E   1.30f    /* 범퍼는 때려서 보낸다 */
#define PB_FLIP_L   104.0f
#define PB_FLIP_W   14.0f
#define PB_FLIP_STEP 15.0f   /* 걸음당 도는 각(도) */
#define PB_DRAIN_X  44.0f    /* 이 폭 안으로 내려가면 빠진다 */
/* 🚨 날개 축은 **벽에 붙여야** 한다. 처음엔 CX±76 에 뒀는데 그 높이의 벽이
 * CX±141 이라 바깥에 65px 짜리 통로가 생겼고, 공이 날개를 거들떠보지도 않고
 * 그리로 흘러내렸다. 축을 원 위에 얹으면 바깥길이 아예 없어진다.
 * sqrt(206^2 - 150^2) = 141.2 — 축 높이에서 원이 여기까지 벌어져 있다. */
#define PB_RING_W   6
/* 안쪽 면이 공이 닿는 자리에 오게 = (PB_WALL + PB_BR + 선굵기) * 2 */
#define PB_RING_D   ((int)((PB_WALL + PB_BR + PB_RING_W) * 2.0f))
/* 🚨 한 걸음(20ms)을 셋으로 쪼갠다. 날개는 걸음당 15도씩 도는데 끝이
 * 피벗에서 104px 이라 한 걸음에 27px 을 쓸고 지나간다 — 공(지름 13)보다
 * 크다. 끝 각도로만 판정하면 **올린 날개가 공을 뚫고 지나가** 아무 일도
 * 안 일어난다. 그게 "판정이 이상하다" 의 정체다. 쪼개면 9px 씩이라 닿는다. */
#define PB_SUB      3
#define PB_FLIP_PX  141.0f
#define PB_FLIP_PY  150.0f
#define PB_BALLS    3

typedef struct {
    float px, py;            /* 축 */
    float rest, up;          /* 쉴 때 · 올렸을 때 각(도, y 아래가 양수) */
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
static bool  s_pb_tup[PB_TGT_N];          /* 아직 서 있나 */
static lv_obj_t *s_pb_tgt[PB_TGT_N];
static int         s_brick_n, s_left_cnt;
static lv_timer_t *s_loop;
static lv_obj_t   *s_score;

static void show_menu(void);

/* 화면이 뜨자마자 시작하면 준비가 안 된 채로 죽는다. 첫 탭을 기다린다. */
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

/* 첫 탭이면 시작만 하고 그 입력은 게임에 넘기지 않는다 */
static bool consume_start_tap(void)
{
    if (!s_pending_cb) return false;
    ESP_LOGI("game", "시작 탭 — 타이머 세움 (기울기모드=%d)", (int)s_tiltmode);
    if (s_ready_lbl) { lv_obj_delete(s_ready_lbl); s_ready_lbl = NULL; }
    /* 지금 자세를 수평으로 기억한다.
     * 터치에서만 온다 — 조작은 이미 세어졌다(깨울 필요가 없다) */
    if (!port_imu_accel(&s_g0x, &s_g0y)) { s_g0x = s_g0y = 0; }
    s_g0_set = true;
    s_loop = lv_timer_create(s_pending_cb, s_pending_ms, NULL);
    s_pending_cb = NULL;
    return true;
}

/* ── 공통 ────────────────────────────────────────────────────── */

static void stop_loop(void)
{
    if (s_loop) { lv_timer_delete(s_loop); s_loop = NULL; }
}

#define POP_MAX  32
#define POP_D    78            /* 방울 지름 — 손끝보다 커야 만지는 맛이 난다 */
#define POP_R    178           /* 방울 중심이 이 안에 있으면 깐다 */

static lv_obj_t *s_pop[POP_MAX];
static uint8_t   s_popped[POP_MAX];
static int       s_pop_n, s_pop_left;
static uint32_t  s_pop_seed = 2463534242u;
static lv_obj_t *s_pop_lbl;
static int       s_refill_in;  /* 0 이상이면 그만큼 뒤에 다시 채운다 */

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
    /* 지운 객체를 가리키던 것들을 전부 끊는다. 안 끊으면 다음 판을 세우기
     * 전에 누가 건드릴 때 해제된 메모리를 밟는다. */
    s_score   = NULL;
    s_paddle  = NULL;
    s_tilt_btn = NULL;
    s_tilt_lbl = NULL;
    s_tiltmode = true;   /* 기본은 기울기 */
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

/* ── 화면 갈아엎기는 이벤트 밖에서 ────────────────────────────
 * 메뉴 버튼과 뒤로 버튼은 s_root 의 자식이다. 그 버튼의 이벤트 안에서
 * lv_obj_clean(s_root) 을 부르면 "지금 처리 중인 그 버튼"이 해제되고,
 * LVGL 이 돌아와 이미 없는 객체를 밟는다. 게임 한두 판 만에 뻗던 원인.
 * 그래서 실제 전환은 1ms 짜리 일회용 타이머로 이벤트 밖에 내보낸다. */
/* 🚨 예전엔 port_tone_enable(true) 만 부르고 끄는 데가 없었다. 벽돌 하나를
 * 깬 순간부터 코덱이 열린 채 사각파가 계속 나갔다 — 게임 내내 스피커·I2S·
 * 앰프가 구동된다. 의도는 "톡" 한 번이었다. 일회용 타이머로 끊는다. */
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
    if (s_defer_fn) return;          /* 연타로 두 번 들어오는 걸 막는다 */
    s_defer_fn = fn;
    lv_timer_t *t = lv_timer_create(defer_cb, 1, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void do_back(void)
{
    clear_board();
    launcher_handle_show(true);      /* 메뉴에선 손잡이를 되돌린다 */
    show_menu();
}

static void orb_menu(void);

/* 🚨 천체의 뒤로가기가 게임 메뉴로 나갔다(0911 지적). 같은 판(app_games.c)을
 * 세 앱이 나눠 쓰는 탓인데, 뒤로가기는 **판이 아니라 들어온 문**을 따라가야
 * 한다. 그래서 돌아갈 데를 단추에 들려 보낸다. */
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

/* 🚨 자리를 판마다 고른다. 기본은 맨 위 바깥쪽(-74,-196)인데, **벽을 그린
 * 판(벽돌·핀볼)에서는 그 자리가 벽 위에 걸친다**(0911 지적: "벽이랑 벽돌,
 * 그리고 버튼"). 벽은 판정이 걸린 물건이라 비켜줄 수 없으니 단추가 안으로
 * 들어온다. 64x34 짜리가 반지름 R 안에 다 들어오려면
 *     (|dx|+32)^2 + (|dy|+17)^2 <= R^2
 * 여야 한다. 벽돌 벽이 제일 좁아서(206) 거기에 맞춘 (∓40, -174) 를 쓴다. */
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

/* ── 기울기 모드 ─────────────────────────────────────────────
 * 판을 손으로 돌리는 대신 배지를 기울여 굴린다. 스트랩에 끼워 손목에
 * 차고 있으면 이쪽이 훨씬 낫다 — 화면을 손가락이 안 가린다.
 * 같은 버튼을 다시 누르면 터치로 돌아온다. */
static void tilt_paint(void)
{
    if (!s_tilt_btn) return;
    /* 기본이 기울기다. 버튼은 "손으로 돌리기" 로 되돌리는 문이라,
     * 켜져 있을 때가 아니라 꺼져 있을 때(=터치 모드) 를 표시한다. */
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
    s_tilt0_set = false;          /* 켤 때의 자세를 새로 잡는다 */
    tilt_paint();
}

static void add_tilt_btn_xy(int dx, int dy)
{
    /* 🚨 기본을 기울기로 둔다. 스트랩에 끼워 손목에 차고 하는 물건이라
     * 손가락으로 판을 돌리면 화면을 제 손이 가린다. */
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

/* ── 벽돌깨기 ────────────────────────────────────────────────
 * 벽돌은 위쪽에 무더기로, 내 판은 아래쪽 호를 따라서만 움직인다.
 * 판은 다이얼처럼 돌린 만큼 움직이고, 아래 반원 밖으로는 못 나간다. */

/* 벽돌은 네모로, 위쪽에 줄지어 놓는다. 줄마다 원이 허락하는 폭을 꽉 채우되
 * 벽돌 자체는 직선이다. 판정도 네모라 단순하다. */
#define BRK_H      26
#define BRK_TOP    80           /* 첫 줄 y — 위쪽 단추(y 42~76) 아래 */
#define BRK_GAP    4
#define PADDLE_R   198.0f
#define PAD_HALF   26.0f
#define PAD_MIN    128.0f
#define PAD_MAX    232.0f
#define BALL_R     8.0f
/* 벽·판의 **안쪽 면**이 공이 닿는 자리(PADDLE_R + BALL_R = 206)에 오게.
 * lv_arc 의 안쪽 면 = 크기/2 - 선굵기. */
#define BRK_WALL_W 4
#define BRK_PAD_W  16
#define BRK_WALL_D ((int)((PADDLE_R + BALL_R + BRK_WALL_W) * 2.0f))
/* 벽돌이 들어갈 수 있는 반지름. 벽 안쪽 면(206)에서 4px 물러선다 —
 * 🚨 딱 붙이면 그림이 닿아 보이고, 공이 벽과 벽돌을 **같은 걸음에** 건드릴
 * 여지가 생긴다. 그때 판정이 두 번 뒤집혀 엉뚱한 데로 튄다. */
#define BRK_FIT    (PADDLE_R + BALL_R - 4.0f)
#define BRK_PAD_D  ((int)((PADDLE_R + BALL_R + BRK_PAD_W) * 2.0f))
/* 프레임당 이동량(20ms 주기 = 50fps). 5.0 이면 초당 250px 쯤 —
 * 25ms/4.5 이던 예전보다 약 40% 빠르다. */
static uint32_t s_brk_last, s_brk_acc;   /* 흐른 시간을 모아 걸음 수로 바꾼다 */
#define BALL_RAMP  1.015f       /* 판에 맞을 때마다 조금씩 */

/* ── 다섯 단계 ────────────────────────────────────────────────
 * 🚨 **속도 하나로 어렵게 만들지 않는다.** 물리는 빠른 공을 견딘다 — 벽돌이
 * 26px 두께라 한 걸음에 42px 이상 가야 뚫리고, 판은 반지름으로 잡아 통과
 * 자체가 불가능하고, 걸음이 시간 기준이라 프레임이 밀려도 안 뚫린다.
 * 못 버티는 건 **사람 손목**이다. 위에서 판까지 5.0 이면 1.6초, 12 면
 * 0.66초인데 조작이 터치가 아니라 기울기라 손목을 실제로 돌려야 하고 판은
 * 아래쪽 104도 안에서만 움직인다. 0.66초는 어려운 게 아니라 운이 된다.
 * 그래서 속도는 9.0 까지만 쓰고 나머지를 네 손잡이로 나눠 올린다.
 *
 * 🚨 시작 속도와 상한을 **둘 다** 정해야 한다. 공은 판에 맞을 때마다
 * 1.5%씩 빨라지다 상한에서 멈춘다(BALL_RAMP). 상한만 올리면 단계마다 초반이
 * 똑같고, 시작만 올리면 몇 번 튀자마자 상한에 붙어 차이가 안 난다. */
#define BRK_LEVELS 5
typedef struct {
    float   spd0, spdmax;   /* 시작 속도 · 상한 (걸음당 px) */
    float   pad_half;       /* 판 반각(도). 줄이면 판이 짧아진다 */
    uint8_t rows;           /* 벽돌 줄 수 (3 또는 4) */
    uint8_t pattern;        /* 벽돌 배치 0=꽉 1=체크 2=가운데빔 */
    uint8_t tough_rows;     /* 위에서 이만큼 줄은 두 번 맞아야 깨진다 */
    float   obs_dps;        /* 도는 방해물 각속도(걸음당 도). 0 이면 없다 */
} brk_level_t;

/* 🚨 뒷단계가 앞단계보다 **빨리 끝나면** 안 된다. 배치를 성기게 하는 것만으로
 * 난이도를 올리면 벽돌이 줄어 판이 짧아진다(첫 판에서 L4 가 8개, L1 이
 * 17개였다 — 거꾸로였다). 성기게 만드는 단계엔 줄을 하나 더 준다. */
static const brk_level_t BRK_LV[BRK_LEVELS] = {
    /* 시작  상한   판반각 줄 배치 단단 방해물 */
    { 5.0f, 6.5f, 26.0f, 3, 0, 0, 0.0f },
    { 5.4f, 7.0f, 24.0f, 4, 0, 1, 0.0f },
    { 5.8f, 7.5f, 22.0f, 4, 1, 2, 1.2f },
    { 6.4f, 8.2f, 20.0f, 4, 2, 2, 1.8f },
    { 7.0f, 9.0f, 18.0f, 4, 0, 2, 2.6f },
};
static int   s_level;                    /* 0..4 */
static float s_pad_half = 26.0f;         /* 지금 단계의 판 반각 */
static float s_spd0, s_spdmax;




static lv_area_t s_brect[BRK_N];
static float     s_bx, s_by, s_vx, s_vy, s_pad_ang;
static float     s_spd;   /* 지금 목표 속도. 판에 맞을 때마다 조금씩 는다 */

static void paddle_draw(void)
{
    /* LVGL 호는 3시가 0도. 우리 각도는 12시 기준이라 90 을 뺀다. */
    float g = s_pad_ang - 90.0f;
    if (g < 0) g += 360.0f;
    lv_arc_set_bg_angles(s_paddle, (int32_t)(g - s_pad_half + 360) % 360,
                                   (int32_t)(g + s_pad_half) % 360);
}

/* 검증용 — 공이 어디 있나. 픽셀로 찾으면 흰 판까지 잡혀서 못 쓴다. */
void brk_debug_ball(float *x, float *y) { if (x) *x = s_bx; if (y) *y = s_by; }

/* 공을 딱 한 걸음(20ms 어치) 움직인다. 아래 brk_step 이 몇 번 부를지 정한다. */
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

    /* 벽돌 — 네모라 판정이 단순하다 */
    bool hit_brick = false;
    for (int i = 0; i < s_brick_n; i++) {
        if (!s_alive[i]) continue;
        const lv_area_t *a = &s_brect[i];
        if (s_bx < a->x1 - BALL_R || s_bx > a->x2 + BALL_R ||
            s_by < a->y1 - BALL_R || s_by > a->y2 + BALL_R) continue;

        /* 🚨 단단한 벽돌은 한 번 맞으면 테두리만 벗고 버틴다. 안 깨져도
         * 튕기는 건 똑같다 — 안 튕기면 그 안에 갇힌다. */
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
        /* 어느 면으로 들어왔는지 보고 그 축만 뒤집는다.
         * 🚨 **겹친 만큼 밀어내야 한다.** 안 밀면 단단한 벽돌(안 없어진다)
         * 안에 남아서 다음 걸음에 또 뒤집힌다 — 두 번 뒤집히면 도로
         * 들어가는 꼴이라 벽돌을 뚫고 지나간 것처럼 보인다. */
        float o_r = a->x2 + BALL_R - s_bx;      /* 오른쪽으로 빠져나갈 거리 */
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

    /* 도는 방해물 — 구슬끼리 부딪히는 것이라 셈이 짧다 */
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
            /* 🚨 겹친 만큼 반드시 밀어내야 한다. 안 밀면 다음 걸음에도
             * 안에 있어서 계속 뒤집히다 방해물에 붙어 버린다. */
            s_bx = ox + nx * hit;
            s_by = oy + ny * hit;
            blip(700, 25);
            break;
        }
    }

    /* 🚨 한 걸음에 **하나만** 처리한다. 벽돌에서 한 번, 벽에서 또 한 번
     * 뒤집으면 두 번 뒤집힌 것이라 원래 가던 방향으로 되돌아간다. */
    if (!hit_brick && r > PADDLE_R) {
        bool bottom = (ang > PAD_MIN - 8 && ang < PAD_MAX + 8);
        float da = fabsf(ang - s_pad_ang);
        if (da > 180) da = 360 - da;

        if (!bottom) {                       /* 위·옆 벽은 그냥 튕긴다 */
            float nx = dx / r, ny = dy / r;
            float dp = s_vx * nx + s_vy * ny;
            s_vx -= 2 * dp * nx;
            s_vy -= 2 * dp * ny;
            s_bx = CX + nx * (PADDLE_R - 2);
            s_by = CY + ny * (PADDLE_R - 2);
        } else if (da < s_pad_half) {        /* 판에 맞았다 */
            float nx = dx / r, ny = dy / r;
            float dp = s_vx * nx + s_vy * ny;
            s_vx -= 2 * dp * nx;
            s_vy -= 2 * dp * ny;
            /* 판의 어디에 맞았는지에 따라 각도를 살짝 튼다 — 조작감이 산다.
             * 각도만 바꾸고 속도는 아래에서 다시 맞춰준다. */
            float off = (ang - s_pad_ang) / s_pad_half;
            s_vx += off * 1.1f;

            /* 튈 때마다 조금씩 빨라지되 크기는 항상 정해준다. 안 그러면
             * 반사가 쌓이면서 제멋대로 느려지거나 빨라진다. */
            if (s_spd < s_spd0) s_spd = s_spd0;
            s_spd *= BALL_RAMP;
            if (s_spd > s_spdmax) s_spd = s_spdmax;
            float m = sqrtf(s_vx * s_vx + s_vy * s_vy);
            if (m > 0.01f) { s_vx = s_vx / m * s_spd; s_vy = s_vy / m * s_spd; }
            s_bx = CX + nx * (PADDLE_R - 3);
            s_by = CY + ny * (PADDLE_R - 3);
            blip(500, 30);
        } else if (r > 224) {                /* 놓쳤다 */
            lv_obj_add_flag(s_ball, LV_OBJ_FLAG_HIDDEN);
            stop_loop();
            /* 🚨 기체가 남았으면 **그 단계를 그대로** 다시 쌓는다. 다 쓰면
             * 그때 1단계로 돌아간다 — 그게 판이 끝나는 자리다. */
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

/* 🚨 예전엔 타이머가 불릴 때마다 딱 한 걸음씩 갔다. LVGL 타이머는 화면
 * 그리기가 길어지면 늦게 오고, 밀렸다가 몰아서 오기도 한다 — 그래서 공이
 * 버벅이다 튀는 느낌이 났다(0908 실기). 진짜 흐른 시간을 재서 그만큼
 * 걸음을 나눠 밟는다. 걸음 자체는 20ms 고정이라 충돌 판정은 그대로다. */
/* ── 기울이는 것도 조작이다 ──────────────────────────────────
 * 🚨 런처는 `lv_display_get_inactive_time()` 으로 무동작을 재는데, 그건
 * **터치만** 조작으로 친다. 기울여 노는 게임은 화면을 안 만지니 한창 굴리는
 * 중에 화면이 꺼졌다(0909 지적).
 * 자세가 실제로 바뀌는 동안만 깨워둔다 — 책상에 내려놓으면 안 움직이니
 * 평소처럼 꺼진다. 🚨 기울기를 쓰는 판에서만 부를 것. 터치로 모는 판은
 * 터치가 이미 조작으로 세므로 여기 손댈 이유가 없다. */
static void tilt_is_input(float lat)
{
    static float last; static bool have;
    if (have && fabsf(lat - last) < 25.0f) return;   /* 손떨림은 조작이 아니다 */
    last = lat; have = true;
    lv_display_trigger_activity(NULL);
}

/* 기울기로 판을 몬다. 켤 때의 자세가 한가운데다 — 어떤 자세로 들고 있든
 * 거기서부터 재니까 누워서도 서서도 쓸 수 있다. */
static void tilt_drive_paddle(void)
{
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) return;
    /* 🚨 IMU 축은 화면 축과 90도 돌아가 있고(구슬에서 확인), 판 각도는
     * 커질수록 왼쪽으로 간다(PAD_MIN 128 이 오른쪽 아래, PAD_MAX 232 가
     * 왼쪽 아래다). 둘을 맞춰야 기울인 쪽으로 판이 간다.
     * 우로 기울이면 gy 가 음수 → 각도가 줄어 오른쪽으로. */
    float lat = gy;
    tilt_is_input(lat);          /* 기울기 모드에서만 오는 길이다 */
    if (!s_tilt0_set) {
        s_tilt0 = lat; s_tilt0_set = true;
        ESP_LOGI("game", "기울기 기준 잡음 lat=%.1f", lat);
    }
    float d = (lat - s_tilt0) * 0.11f;          /* 기울인 만큼 각도로 */
    float want = (PAD_MIN + PAD_MAX) * 0.5f + d;
    if (want < PAD_MIN) want = PAD_MIN;
    if (want > PAD_MAX) want = PAD_MAX;
    /* 곧바로 따라가면 손떨림까지 따라간다. 조금 늦게 붙는다. */
    s_pad_ang += (want - s_pad_ang) * 0.35f;
    paddle_draw();
}

static void brk_step(lv_timer_t *t)
{
    (void)t;
    static uint32_t nstep;
    if ((nstep++ % 200) == 0)
        ESP_LOGI("game", "브릭 %u걸음째 기울기모드=%d 판각=%.0f",
                 (unsigned)nstep, (int)s_tiltmode, s_pad_ang);
    if (s_tiltmode) tilt_drive_paddle();
    /* 화면이 꺼졌으면 공은 굴릴 이유가 없다. 예전엔 검은 덮개 뒤에서
     * 초당 50번 계속 돌고 벽에 맞을 때마다 소리까지 냈다. */
    if (launcher_screen_is_off()) { port_tone_enable(false); port_tone_hold(false); s_brk_last = 0; return; }
    /* 🚨 효과음마다 코덱을 여닫으면 첫 소리가 통째로 빠진다(0908 실기).
     * 판이 도는 동안엔 열어둔다 — 이미 열려 있으면 하는 일이 없다. */
    port_tone_hold(true);

    uint32_t now = lv_tick_get();
    if (!s_brk_last) { s_brk_last = now; brk_phys(); return; }
    uint32_t el = now - s_brk_last;
    if (el > 200) el = 200;          /* 앱 전환처럼 오래 멈췄으면 순간이동 금지 */
    s_brk_acc += el;
    s_brk_last = now;

    int steps = 0;
    while (s_brk_acc >= 20 && steps < 5) {
        s_brk_acc -= 20;
        brk_phys();
        steps++;
        /* 공을 놓쳤거나 다 깼으면 루프가 이미 멈췄다 — 더 밟지 않는다 */
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

    if (s_tiltmode) return;      /* 기울기로 모는 중엔 손가락이 안 뺏는다 */
    /* 다이얼처럼 돌린 만큼. 다만 아래쪽 호를 벗어나지 못한다. */
    float d = a - s_grab_ang;
    while (d > 180)  d -= 360;
    while (d < -180) d += 360;
    s_pad_ang = s_grab_pad + d * 1.5f;
    if (s_pad_ang < PAD_MIN) s_pad_ang = PAD_MIN;
    if (s_pad_ang > PAD_MAX) s_pad_ang = PAD_MAX;
    paddle_draw();
}

/* 검증용 — 벽돌깨기를 곧바로 굴린다(탭을 기다리지 않는다).
 * 게임이 도는 동안 타이머가 실제로 몇 ms 마다 오는지 재려면 필요하다.
 * 그 값이 공 속도를 정한다. */
/* 검증용 — 잡은 "수평" 기준과 지금 값을 밖에서 대조할 수 있게 낸다.
 * 🚨 IMU 를 재웠다 깬 직후 기준을 잡으면 엉뚱한 데 박히는데, 눈으로는
 * "판이 안 움직인다" 로만 보인다. 숫자로 봐야 잡힌다. */
void games_debug_tilt0(float *base, int *set)
{
    if (base) *base = s_tilt0;
    if (set)  *set  = s_tilt0_set ? 1 : 0;
}

/* 검증용 — 천체를 곧바로 띄운다(고르는 화면을 건너뛴다) */
void games_debug_play_orb(void)
{
    clear_board();
    launcher_handle_show(true);
    s_loop = orb_start(s_root, ORB_EARTH);
}



/* 검증용 — 벽돌깨기를 곧바로 굴린다(탭을 기다리지 않는다).
 * 게임이 도는 동안 타이머가 실제로 몇 ms 마다 오는지 재려면 필요하다.
 * 그 값이 공 속도를 정한다. */
void games_debug_play_bricks(void);   /* 아래에 있다 */

/* 검증용 — 벽돌깨기를 원하는 단계로 곧바로 굴린다. 다섯 판을 사람 없이
 * 돌려보려면 이 문이 있어야 한다. */
void games_debug_brk_level(int lv)
{
    s_level = (lv < 0) ? 0 : (lv >= BRK_LEVELS ? BRK_LEVELS - 1 : lv);
    games_debug_play_bricks();
}

/* 검증용 — 지금 판이 표대로 세워졌는지 밖에서 대조한다. */
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

/* 단계를 넘기거나 놓쳤을 때 판을 다시 세운다. 🚨 반드시 defer 로 부를 것 —
 * 타이머 콜백 안에서 lv_obj_clean 을 하면 지금 도는 그 객체를 지운다. */
static void brk_rebuild(void)
{
    clear_board();
    launcher_handle_show(false);
    brk_start();
}

void brk_start(void)
{
    /* 게임 중엔 손잡이를 숨기므로 가운데에서 시작해도 안 겹친다.
     * 판은 아래 한가운데, 공은 그 바로 위. */
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
    s_brk_last = s_brk_acc = 0;                      /* 시간 누적도 처음부터 */
    s_spd = s_spd0;                                  /* 판마다 처음부터 */
    s_vx = s_spd0 * 0.6f; s_vy = -s_spd0 * 0.8f;     /* 3:4 방향 */
    s_brick_n = 0;

    lv_obj_t *pad = lv_obj_create(s_root);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 466, 466);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(pad, brk_touch, LV_EVENT_RELEASED, NULL);

    /* 위쪽에 세 줄 또는 네 줄. 줄마다 원이 허락하는 폭을 재서 개수와 너비를
     * 맞춘다. 🚨 네 줄일 땐 **위로** 올려서 시작한다. 아래로 한 줄 더 붙이면
     * 맨 아랫줄이 화면 한가운데까지 내려와 공이 판까지 오는 거리가 확 줄고,
     * 그건 난이도가 아니라 반응할 시간을 뺏는 것이다. */
    static const uint32_t COL[BRK_ROWS] = { 0x7FB0FF, 0x5BD48A, 0xE0A33A, 0xC98BE0 };
    int rows = (L->rows < 3) ? 3 : (L->rows > BRK_ROWS ? BRK_ROWS : L->rows);
    /* 🚨 단추가 벽을 피해 안으로 들어왔다(GBTN_IN_DY = -174 → y 42~76).
     * 줄은 그 아래부터다. 예전 값(60)이면 단추가 첫 줄을 덮는다. */
    int top  = BRK_TOP;
    for (int row = 0; row < rows; row++) {
        int y = top + row * (BRK_H + BRK_GAP);
        /* 🚨 **아래 모서리로 쟀던 게 틀렸다.** 줄은 전부 가운데보다 위에
         * 있으니 원에 먼저 부딪히는 쪽은 **위 모서리**다. 아래 모서리로
         * 재면 줄 끝의 위쪽 귀퉁이가 원 밖으로 한참 나간다 — 첫 줄은
         * r=225 까지 나갔다(벽은 206). 벽을 긋고 나서야 보였다(0911 지적:
         * "벽돌이랑 겹치잖아"). 두 모서리 중 **먼 쪽**으로 잰다. */
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
            /* 단계마다 배치를 바꾼다. 같은 판을 더 빠르게가 아니라
             * 다른 판이 되도록. */
            bool skip = false;
            if (L->pattern == 1)      skip = ((row + k) & 1);          /* 체크무늬 */
            else if (L->pattern == 2) skip = (n >= 5 && k == n / 2);   /* 가운데 한 칸 빔 */
            if (skip) continue;

            int x = x0 + k * (bw + BRK_GAP);
            lv_obj_t *b = lv_obj_create(s_root);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, bw, BRK_H);
            lv_obj_set_pos(b, x, y);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(COL[row]), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            /* 단단한 벽돌은 흰 테두리를 두른다. 한 번 맞으면 테두리를 벗는다 —
             * 보이는 것만 바뀌고 그리는 값은 그대로다. */
            s_hp[s_brick_n] = (row < L->tough_rows) ? 2 : 1;
            if (s_hp[s_brick_n] > 1) {
                /* 🚨 remove_style_all 을 한 객체는 border_side 가 NONE 이라
                 * 굵기와 색만 줘도 아무것도 안 그려진다. 면을 켜야 보인다. */
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

    /* 🚨 어디까지가 벽이고 어디가 구멍인지 안 보였다(0911 지적). 판만 그려
     * 놓으니 나머지가 전부 똑같이 비어 있어서, 위·옆으로 튕겨 나오는 것도
     * 아래로 빠지는 것도 다 우연처럼 보인다. **막힌 만큼만** 얇게 긋는다.
     * 막힌 구간은 판정과 같은 자리다 — brk_phys 의 `bottom` 이 거짓인 곳,
     * 즉 12시 기준 240도에서 시계방향으로 120도까지. lv_arc 는 3시가 0도라
     * 90 을 빼면 150 → 30 이다. */
    lv_obj_t *wall = lv_arc_create(s_root);
    lv_obj_remove_style_all(wall);
    lv_obj_remove_flag(wall, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wall, BRK_WALL_D, BRK_WALL_D);
    lv_obj_center(wall);
    lv_arc_set_bg_angles(wall, (int32_t)(PAD_MAX + 8 - 90), (int32_t)(PAD_MIN - 8 - 90));
    lv_arc_set_value(wall, 0);
    lv_obj_set_style_arc_width(wall, BRK_WALL_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(wall, lv_color_hex(0x3A3A46), LV_PART_MAIN);

    /* 🚨 판도 같은 안쪽 면에 맞춘다. 예전 크기(422)면 안쪽 면이 r=195 라
     * 공(중심 198, 반지름 8)이 판에 절반쯤 파묻힌 채로 튕겼다. */
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
    /* 🚨 놓아주지 않으면 LVGL 이 새 객체를 (0,0) 에 둔다 — "tap to start" 가
     * 떠 있는 동안 공이 왼쪽 위 구석에 앉아 있었다. 첫 걸음이 오기 전까지
     * 그대로다. 핀볼은 만들자마자 놓고 있었는데 여기만 빠졌다. */
    put(s_ball, s_bx, s_by);
    s_score = make_score();
    lv_label_set_text_fmt(s_score, "L%d  0  o%d", s_level + 1, s_brk_life);
    add_back_xy(do_back, -GBTN_IN_DX, GBTN_IN_DY);
    add_tilt_btn_xy(GBTN_IN_DX, GBTN_IN_DY);
    arm_start(brk_step, 20);
}

/* ── 3) 구슬 미로 ────────────────────────────────────────────
 * 기울이면 구슬이 굴러간다. 구멍에 넣으면 다음 구멍이 다른 자리에 생긴다.
 * IMU 가 없는 시뮬에서는 손가락 위치를 기울기 대신 쓴다. */

static lv_obj_t *s_marble, *s_hole;
static float     s_mx, s_my, s_mvx, s_mvy, s_hx, s_hy;
static int       s_got;
static float     s_fake_gx, s_fake_gy;

static void new_hole(void)
{
    float a = (float)(rand() % 360) * DEG2RAD;
    float r = 50 + (float)(rand() % 120);       /* 가장자리까지 안 밀어붙인다 */
    s_hx = CX + cosf(a) * r;
    s_hy = CY + sinf(a) * r;
    put(s_hole, s_hx, s_hy);
}

/* 검증용 — 구슬이 어디 있나. 기울기 축이 안 뒤집혔는지 재려면 필요하다. */
void mz_debug_ball(float *x, float *y) { if (x) *x = s_mx; if (y) *y = s_my; }

static void maze_step(lv_timer_t *t)
{
    (void)t;
    /* 화면이 꺼졌으면 공은 굴릴 이유가 없다. 예전엔 검은 덮개 뒤에서
     * 초당 50번 계속 돌고 벽에 맞을 때마다 소리까지 냈다. */
    if (launcher_screen_is_off()) { port_tone_enable(false); port_tone_hold(false); return; }
    port_tone_hold(true);
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) { gx = s_fake_gx; gy = s_fake_gy; }
    tilt_is_input(gy);           /* 구슬은 기울기로만 논다 */

    /* 시작할 때의 자세를 빼서 "그 자세 기준으로 얼마나 기울였나"만 남긴다.
     * 이러면 눕혀 놔도 세워 들어도 시작 자세가 곧 수평이 된다. */
    if (s_g0_set) { gx -= s_g0x; gy -= s_g0y; }

    /* mg 단위를 가속도로. 값이 커서 많이 줄인다.
     * 부호: 0905 에 기준 방향을 MADCTL 로 180도 뒤집으면서(스트랩 착용 방향)
     * IMU 축과 화면 축의 대응도 같이 뒤집혔다. IMU 는 기판에 고정이라
     * 화면을 돌려도 안 따라온다 — 여기서 손으로 맞춰준다. */
    /* 🚨 부호가 아니라 축이 통째로 어긋나 있었다. IMU 는 기판에 붙어 있고
     * 화면 축과 90도 돌아가 있다 — 부호만 뒤집어봐야 계속 옆으로 굴렀다
     * (0908 에 그렇게 두 번 틀렸다).
     *
     * 0909 실기 관찰 네 가지로 역산했다(고치기 전 상태):
     *     우로 기울임 → 아래로   ⇒ gy 가 화면 세로를 몰고, 우 = gy 음수
     *     좌로 기울임 → 위로
     *     앞으로 기울임 → 왼쪽   ⇒ gx 가 화면 가로를 몰고, 앞 = gx 양수
     *     안으로 기울임 → 오른쪽
     * 즉 gx 는 세로(앞뒤), gy 는 가로(좌우) 를 맡아야 한다. 서로 바꾼다.
     *
     * 바뀐 뒤 기대: 우 → 우 · 좌 → 좌 · 앞 → 위(앞) · 안 → 아래(안).
     * 통을 기울이면 내용물이 그쪽으로 쏠린다 — 사람이 기대하는 방향이다. */
    s_mvx -= gy * 0.00032f;
    s_mvy -= gx * 0.00032f;
    s_mvx *= 0.965f;                 /* 구르는 마찰 — 세게 걸어 가장자리에 안 붙게 */
    s_mvy *= 0.965f;

    /* 너무 빠르면 구멍을 뛰어넘는다. 위를 막는다. */
    float sp = sqrtf(s_mvx * s_mvx + s_mvy * s_mvy);
    if (sp > 7.0f) { s_mvx = s_mvx / sp * 7.0f; s_mvy = s_mvy / sp * 7.0f; }
    s_mx += s_mvx;
    s_my += s_mvy;

    /* 가장자리에서 튕긴다 */
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
    if (hdx * hdx + hdy * hdy < 30 * 30) {      /* 구멍 판정을 넉넉히 */
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
    /* 시뮬은 손가락 쪽으로 끌리게. 위에서 gx/gy 를 맞바꿨으니 여기도 바꾼다 */
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
    put(s_marble, s_mx, s_my);   /* 벽돌과 같은 이유 — 안 놓으면 (0,0) 이다 */
    new_hole();
    s_score = make_score();
    lv_label_set_text(s_score, "0");
    add_back();
    arm_start(maze_step, 25);
}

/* ── 4) 핀볼 ─────────────────────────────────────────────────
 * 둥근 화면이 곧 테이블이다. 핀볼 경기장은 원래 둥근 접시라 네모로 만들면
 * 오히려 어색하다 — 벽돌깨기의 링 패들처럼 원형이 규칙을 더 좋게 만드는 쪽.
 *
 * 🚨 진짜 테이블을 축소해 넣지 않았다. 요소를 여덟 개쯤 넣으면 하나가
 * 20~40px(2~4mm)이 되어 뭐가 뭔지 구분이 안 된다. 여섯 개만 두고 하나를
 * 크게 키웠다 — 범퍼 셋, 날개 둘, 빠지는 구멍 하나.
 *
 * 🚨 손이 계속 붙어 있어야 게임이다. 던지고 구경하는 물건이 되지 않게
 * 날개 둘 말고도 **배지를 기울여 공을 밀 수 있다**(nudge). 진짜 핀볼에서
 * 대(臺)를 툭툭 치는 그 기술이고, 여기선 그게 진짜 기울기다. */

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

/* 날개에 맞았나. 선분에서 가장 가까운 점을 찾아 그 법선으로 튕긴다.
 * 🚨 날개가 **도는 중이면 그 속도를 얹어야** 한다. 안 얹으면 공이 날개에
 * 그냥 부딪혀 떨어질 뿐이라 핀볼이 안 된다 — 쳐서 올리는 맛이 전부다. */
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
        float w = omega * DEG2RAD;              /* 걸음당 라디안 */
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
    /* 다음 공은 위쪽에서 떨어진다 */
    s_pb_x = CX + 96.0f; s_pb_y = CY - 150.0f;
    s_pb_vx = -1.2f; s_pb_vy = 0.6f;
    lv_label_set_text_fmt(s_score, "%d  o%d", s_pb_pts, s_pb_left);
}

static void pb_phys(void)
{
    /* 🚨 기울기가 곧 테이블 경사다. 축 대응은 이 배지에서 잰 값이 하나뿐이고
     * 물·구슬이 이미 그걸 쓴다:
     *     화면 오른쪽 = -ay      화면 아래 = -ax
     * 핀볼만 `+gy` 를 화면 오른쪽으로 썼다. 그래서 **좌우가 뒤집혔고**
     * (0911 제보: "왼쪽으로 기울이면 오른쪽"), 세로는 기울기를 아예 안 봐서
     * 앞뒤로 기울여도 공이 반응하지 않았다.
     * 🚨 예전엔 시작할 때의 자세를 0 으로 잡았다(s_pb_tilt0). 테이블 경사는
     * 들고 있는 자세와 무관한 절대값이라 그러면 안 된다 — 비스듬히 들고
     * 시작하면 그 비스듬함이 '수평' 이 돼버린다. 뺐다. */
    float gx, gy;
    if (port_imu_accel(&gx, &gy)) {
        tilt_is_input(gy);
        s_pb_vx += -gy * PB_TILT;
        s_pb_vy += -gx * PB_TILT;
    }

    /* 🚨 기본 경사는 남긴다. 기울기만으로 굴리면 평평하게 들었을 때 공이
     * 떠 있다 — 진짜 핀볼 대(臺)도 늘 앞으로 기울어 있다. */
    s_pb_vy += PB_GRAV;
    s_pb_vx *= PB_DAMP;
    s_pb_vy *= PB_DAMP;
    float sp = sqrtf(s_pb_vx * s_pb_vx + s_pb_vy * s_pb_vy);
    if (sp > PB_MAXV) { s_pb_vx = s_pb_vx / sp * PB_MAXV; s_pb_vy = s_pb_vy / sp * PB_MAXV; }

    /* 날개를 목표 각으로 옮긴다. 이번 걸음에 얼마나 돌았는지가 손맛이다. */
    float om[2];
    for (int i = 0; i < 2; i++) {
        float want = s_flip[i].on ? s_flip[i].up : s_flip[i].rest;
        float d = want - s_flip[i].ang;
        if (d >  PB_FLIP_STEP) d =  PB_FLIP_STEP;
        if (d < -PB_FLIP_STEP) d = -PB_FLIP_STEP;
        om[i] = d;
    }
    /* 🚨 날개가 쳐 주는 속도(om)는 **걸음당** 값이라 조각마다 나누면 안 된다.
     * 대신 한 걸음에 한 번만 얹는다 — 조각마다 얹으면 세 배로 날아간다. */
    bool kicked[2] = { false, false };
    for (int k = 0; k < PB_SUB; k++) {
        for (int i = 0; i < 2; i++)
            if (om[i] != 0.0f) { s_flip[i].ang += om[i] / PB_SUB; pb_flip_draw(i); }
        if (!pb_sub(om, kicked)) return;
    }
    put(s_pb_ball, s_pb_x, s_pb_y);
}

/* 한 조각 — 공을 조금 옮기고 그 자리에서 판정한다.
 * false 면 공이 빠진 것이라 그 걸음은 거기서 끝난다. */
static bool pb_sub(const float *om, bool *kicked)
{
    s_pb_x += s_pb_vx / PB_SUB;
    s_pb_y += s_pb_vy / PB_SUB;

    /* 범퍼 — 맞으면 점수가 오르고 세게 튕겨 나간다 */
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

    /* 표적 — 맞으면 눕고 점수가 크다. 넷을 다 눕히면 한꺼번에 다시 선다. */
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
        if (up == 0) {                     /* 다 눕혔다 — 보너스와 함께 다시 */
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

    /* 벽 — 아래 가운데만 뚫려 있다. 거기로 내려가면 공을 잃는다. */
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

/* 화면 왼쪽 절반이 왼 날개, 오른쪽 절반이 오른 날개. 두 손가락도 받는다. */
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

    /* 🚨 벽 그림과 판정이 어긋나 있었다(0911 지적: "부딪히는 판정이 이상").
     * 원을 지름 424 로 잡으면 안쪽 면이 r=206 인데 공 **중심**이 거기서
     * 멈춘다 — 반지름 6.5 짜리 공이 벽을 통째로 덮고 바깥 면까지 넘어간다.
     * 눈에는 공이 벽을 뚫은 것으로 보인다. 안쪽 면을 공이 닿는 자리
     * (PB_WALL + PB_BR)에 맞춘다. lv_arc 의 안쪽 면 = 크기/2 - 선굵기. */
    lv_obj_t *ring = lv_arc_create(s_root);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, PB_RING_D, PB_RING_D);
    lv_obj_center(ring);
    /* 🚨 구멍도 보여야 한다. 360도를 다 그려 놓고 아래 가운데로만 빠지니
     * 멀쩡한 벽을 통과해 사라지는 것처럼 보였다. 실제로 뚫린 만큼만 비운다 —
     * 반각 = asin(구멍폭 / 벽반지름). lv_arc 는 3시가 0도, 시계방향이다. */
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

    /* 🚨 예전엔 큰 범퍼 셋뿐이라 판이 휑했다(0911 제보: "레벨이 너무 심플").
     * 화면은 못 키우니 **알을 줄여 자리를 벌었다** — 공 9→6.5, 범퍼 30→21.
     * 그 자리에 범퍼를 다섯으로 늘리고 표적 넷을 더 놨다. */
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

    /* 표적 넷 — 위쪽 양옆 통로에 둘씩. 거기로 올려 보내야 맞는다. */
    static const float TX[PB_TGT_N] = { -150, -150,  150,  150 };
    static const float TY[PB_TGT_N] = {  -78,  -34,  -78,  -34 };
    for (int i = 0; i < PB_TGT_N; i++) {
        s_pb_tx[i] = CX + TX[i];
        s_pb_ty[i] = CY + TY[i];
        s_pb_tup[i] = true;
        s_pb_tgt[i] = dot(s_root, (int)(PB_TGT_R * 2), 0xFFD24A);
        put(s_pb_tgt[i], s_pb_tx[i], s_pb_ty[i]);
    }

    /* 날개 둘. 쉴 때는 안쪽 아래를 보고, 올리면 위로 친다. */
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
    /* 🚨 공용 점수 자리(+168)는 두 날개 사이 — 공이 빠지는 바로 그 자리다.
     * 글자가 구멍을 가리면 언제 빠지는지 안 보인다. 위로 올린다. */
    lv_obj_align(s_score, LV_ALIGN_CENTER, 0, 116);
    lv_label_set_text_fmt(s_score, "0  o%d", s_pb_left);
    add_back_xy(do_back, -GBTN_IN_DX, GBTN_IN_DY);
    arm_start(pb_step, 20);
}

/* 검증용 — 핀볼을 곧바로 굴린다 */
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

/* 검증용 — 공 자리와 남은 개수 */
void pb_debug(float *x, float *y, int *pts, int *left)
{
    if (x)    *x    = s_pb_x;
    if (y)    *y    = s_pb_y;
    if (pts)  *pts  = s_pb_pts;
    if (left) *left = s_pb_left;
}

/* ── 메뉴 ────────────────────────────────────────────────────── */

static int s_pick;

static void do_pick(void)
{
    clear_board();
    /* 게임(벽돌·구슬·뽁뽁이)은 화면 전체가 조작면이라 손잡이가 조작을 뺏는다.
     * 물·달·지구는 아래쪽이 비어 있어서 손잡이를 남겨도 안 걸린다 —
     * 오히려 없으면 홈으로 나갈 길이 PWR 뿐이라 불편하다. */
    launcher_handle_show(false);   /* 게임은 화면 전체가 조작면이다 */
    switch (s_pick) {
        /* 메뉴에서 고르면 1단계부터, 기체도 새로 */
        case 0: s_level = 0; s_brk_life = BRK_LIVES; brk_start(); break;
        case 1: pb_start();   break;
        case 2: maze_start(); break;
        default: pop_start(); break;
    }
}

static void pick_cb(lv_event_t *e)
{
    /* 여기서 바로 판을 갈아엎으면 지금 눌린 그 버튼이 해제된다 → defer */
    s_pick = (int)(intptr_t)lv_event_get_user_data(e);
    defer(do_pick);
}


/* ── 뽁뽁이 ──────────────────────────────────────────────────
 * 피젯 토이. 이 배지를 상시 켜두는 디지털 명찰로 쓰면 배터리가 못 버틴다
 * (화면이 제일 많이 먹는다). 그래서 "잠깐 꺼내 만지작거리다 넣는" 쓰임을
 * 노렸다. 점수도 규칙도 없다 — 누르면 터지고, 다 터지면 다시 찬다.
 * 타이머는 10Hz 하나뿐이고 하는 일은 화면이 꺼졌나 보는 게 전부다. */
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
    /* 진짜 뽁뽁이도 방울마다 소리가 조금씩 다르다 */
    blip(760 + (pop_rnd() % 620), 22);
    if (s_pop_left == 0) s_refill_in = 7;    /* 0.8초쯤 뒤 다시 찬다 */
}

/* 길게 누르면 다 터뜨리지 않아도 새로 찬다 */
static void pop_long_cb(lv_event_t *e)
{
    (void)e;
    pop_refill();
}

static void pop_step(lv_timer_t *t)
{
    /* 🔋 화면이 꺼졌는데 10Hz 로 계속 깨어날 이유가 없다. 느리게 돌린다.
     * (앱 타이머는 화면이 꺼져도 멈추지 않는다 — 직접 늦춰야 한다.) */
    if (launcher_screen_is_off()) {
        port_tone_enable(false);
        port_tone_hold(false);
        /* 🚨 주기를 바꾸면 lv_timer_handler 가 무한히 다시 돈다(0909).
         * 주기는 그대로 두고 16번에 한 번만 일한다. */
        static uint8_t skip;
        if (++skip % 16) return;
        return;
    }
    port_tone_hold(true);        /* 이미 열려 있으면 하는 일이 없다 */
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

    /* 육각으로 깔면 둥근 화면이 고르게 찬다. 줄마다 반 칸씩 민다. */
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
            /* 누르는 순간 터져야 손맛이 산다 — 떼는 걸 안 기다린다 */
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

    /* 🚨 넷 중 여기만 뒤로가기가 없었다(0911 지적). 나머지 셋은 add_back 을
     * 부르는데 이 함수만 빠져 있었다 — 눈으로는 안 보이는 종류의 누락이다. */
    add_back();

    s_loop = lv_timer_create(pop_step, 120, NULL);
}

static void show_menu(void)
{
    /* 둥근 화면이라 세로로 길게 늘어놓으면 위아래가 잘린다. 2열 격자가 맞다. */
    /* 게임만 남긴다. 물·천체는 홈에서 바로 여는 제 앱이 됐다. */
    static const char *NAME[4] = { "Bricks", "Pinball", "Marble", "Pop" };
    static const uint32_t COL[4] = { 0x2E6E5A, 0x6E2E4A, 0x6E5A2E, 0x4A3A6E };

    lv_obj_t *t = lv_label_create(s_root);
    lv_label_set_text(t, "Games");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    /* 🚨 단추를 넷으로 늘리면서 첫 단추가 -135(y 63~133) 로 올라왔다. 제목이
     * -158(y 75) 이면 그 뒤에 깔려 아예 안 보인다(0911 시뮬 그림에서 발견).
     * -196 은 y 37, 그 자리의 폭이 252px 이라 글자가 안 잘린다. */
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -196);

    /* 🚨 넷으로 늘렸다. 둥근 화면이라 y=±172 에서 쓸 수 있는 폭이 314px 다 —
     * 단추를 260 으로 줄여야 모서리가 안 잘린다. */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(s_root);
        lv_obj_set_size(b, 260, 70);
        lv_obj_set_style_radius(b, 35, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL[i]), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        /* 3줄 x 2칸. 가운데 줄이 제일 넓으니 바깥으로 조금 더 벌린다. */
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
    s_defer_fn = NULL;      /* 미뤄둔 전환이 남으면 지워진 판을 밟는다 */
    launcher_handle_show(true);
    stop_loop();
    water_stop();
    orb_stop();
    port_tone_enable(false);
    port_tone_hold(false);   /* 나가면 코덱을 놓는다 */
    s_root = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x7FB0FF); }

/* ── 홈에서 바로 들어가는 문 ─────────────────────────────────
 * 물과 천체는 게임 메뉴를 거치지 않고 홈에서 바로 연다.
 * 판은 같은 것을 쓴다 — 코드가 두 벌이 되면 한쪽만 고치는 사고가 난다. */
static void enter_water(lv_obj_t *root)
{
    enter(root);              /* 판을 세운다(메뉴가 그려진다) */
    lv_obj_clean(s_root);     /* 메뉴는 걷어낸다 */
    launcher_handle_show(true);
    s_loop = water_start(s_root);
}

/* ── 천체 ────────────────────────────────────────────────────
 * 다섯을 골라 본다. 앱을 다섯 개로 늘리면 홈이 붐비고, 그림·표는 어차피
 * 하나뿐이라 나눌 이유가 없다. 고른 것은 기억해서 다음에 그대로 연다. */
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
    /* 🚨 고르는 화면으로 되돌아갈 데가 있는데 문이 없었다(0911 지적).
     * 게임과 같은 자리에 같은 모양으로 두되, 돌아갈 곳은 **천체 목록**이다. */
    add_back_to(do_orb_back);
}

static void orb_menu(void)
{
    lv_obj_t *t = lv_label_create(s_root);
    lv_label_set_text(t, "Orbit");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -172);

    /* 천체마다 제 빛깔로. 무엇인지 글자 없이도 대충 안다. */
    static const uint32_t COL[ORB_N] = {
        0x4A4A52,   /* 달   회색 */
        0x2A5A7E,   /* 지구 파랑 */
        0x8A5A18,   /* 태양 주황 */
        0x7A5A3A,   /* 목성 갈색 */
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
