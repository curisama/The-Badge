/* 에어마우스. 이 배지를 산 1번 이유.
 *
 * 폰 RDP 에서 터치↔마우스 모드를 계속 토글해야 하는 게 원래 문제였다.
 * BLE HID 로 붙으면 폰이 진짜 마우스로 인식하니 토글 자체가 사라진다.
 *
 * 조작 구역은 두 개다:
 *   가운데 원  — 문지르면 커서 (상대 이동)
 *   가장자리 링 — 돌리면 스크롤
 * 클릭은 터치로 받는다. 물리 버튼은 PWR 이 AXP2101 라 빠르게 못 누른다. */
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
#define PAD_R       150         /* 여기까지가 트랙패드 */
#define RING_R_IN   152         /* 여기부터 스크롤 링 */
#define MOVE_SLOP   3           /* 이만큼 움직이면 '끈 것'으로 본다 */
#define HOLD_MS     500         /* 이만큼 누르고 있으면 우클릭 */
#define WHEEL_DEG   18          /* 링을 이만큼 돌 때마다 한 칸 */
#define TAPDRAG_MS  320         /* 뗀 뒤 이 안에 다시 누르면 끌기로 본다 */
#define TAPDRAG_PX  45          /* 그때 손가락이 이만큼 안에 있어야 한다 */

enum { MODE_NONE, MODE_PAD, MODE_RING, MODE_TWO };

#define TWO_SCROLL_PX  14       /* 두 손가락으로 이만큼 끌 때마다 한 칸 */

static lv_obj_t   *s_pad, *s_dot, *s_state, *s_hint, *s_ring;
static lv_timer_t *s_hold, *s_release, *s_poll;

static lv_point_t s_last;
static int        s_mode;
static bool       s_moved;
static float      s_ring_acc;
static uint32_t   s_prev_ms;
static float      s_last_ang;

/* 탭-드래그: 톡 치고 곧바로 다시 눌러 끌면 버튼을 누른 채로 움직인다.
 * RDP 에서 글자 선택하고 창 끄는 데 이게 없으면 못 쓴다. */
static uint32_t   s_last_release_ms;
static lv_point_t s_last_release_pt;
static bool       s_drag_lock;
static bool       s_two;        /* 이번 터치에서 두 손가락을 본 적 있나 */
static int        s_two_acc;

/* ── 클릭: 눌렀다 떼는 걸 두 리포트로 보낸다 ─────────────────── */

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

/* ── 가속 커브 ───────────────────────────────────────────────
 * 계단식(1x / 1.5x / 2.5x / 3.5x)이면 속도가 단을 넘을 때마다 커서가 툭 튄다.
 * 배율을 속도에 따라 연속으로 올린다: 1.0 에서 시작해 3.5 에서 멈춘다.
 *
 * 그리고 배율을 곱하면 소수점이 남는데, 그냥 버리면 천천히 미는 동안
 * 남은 조각이 계속 사라져 커서가 안 따라온다. 다음 번에 보태준다. */

/* ── 기울기 보정 ─────────────────────────────────────────────
 * 배지를 비스듬히 들거나 거꾸로 쥐어도 "위로 밀면 위로" 가게 만든다.
 * 각도는 천천히 따라가되, 손가락이 닿는 순간 얼어붙는다 —
 * 끄는 도중에 기준이 돌아가면 커서가 휘어버린다. */
static float s_tilt_deg;        /* 지금 기기가 돌아가 있는 각 */
static float s_tilt_lock;       /* 이번 터치에 쓸 각 */
static bool  s_tilt_valid;


/* 기울기 보정은 뺐다. tilt_poll 이 어느 타이머에도 안 걸려 있어 s_tilt_valid
 * 가 영원히 거짓이었고 — 즉 처음부터 안 돌던 죽은 코드였다. 스트랩을 끼워
 * 손목에 고정해 쓰므로 방향이 바뀔 일도 없다. 항등으로 남겨둔다. */
static void rotate_delta(int dx, int dy, int *rx, int *ry)
{
    if (!s_tilt_valid) { *rx = dx; *ry = dy; return; }
    float r = s_tilt_lock * 0.0174533f;
    float c = cosf(r), s = sinf(r);
    *rx = (int)lroundf(dx * c - dy * s);
    *ry = (int)lroundf(dx * s + dy * c);
}

/* ── 왜 뻣뻣했나 ──────────────────────────────────────────────
 * CST9217 이 실제로 내주는 좌표는 0~465 다(부팅 로그 "Resolution X: 466").
 * 1.75인치에 466단계 = 약 266 DPI 로, 노트북 트랙패드(1000~1600)의 1/4~1/6.
 * 판도 지름 44mm 로 좁아 화면을 가로지르려면 게인을 키워야 하는데, 게인은
 * 그 거친 눈금을 같이 곱한다. 그게 계단으로 느껴진 정체다.
 *
 * 그런데 더 큰 범인이 따로 있었다. 예전엔 터치 이벤트 하나에 리포트 하나를
 * 보냈다. 터치는 83Hz(12ms), BLE 연결 간격은 폰이 주는 대로 대개 66Hz(15ms).
 * 둘이 안 나누어떨어져서 어떤 리포트엔 터치 한 걸음, 어떤 리포트엔 두 걸음이
 * 실렸다. 그 박자 어긋남이 곧 울컥거림이었다.
 *
 * 그래서 "이벤트마다 보내기"를 버리고 속도를 매개로 둔다:
 *   터치가 오면  → 순간 속도(카운트/초)를 고쳐 잡고
 *   타이머가     → 시간에 비례한 양을 내보낸다
 * 몇 번 들어왔는지와 무관해지므로 박자 어긋남이 사라진다. 소수점은 누적해
 * 두니 총 이동량도 손실이 없다.
 *
 * 값은 PC 에서 느린/보통/빠른 드래그를 흉내내 고른 것이다. 보통 속도의
 * 흔들림이 예전의 절반이 되고, 빠르게 휙 그을 때는 1.3배 멀리 간다. */

#define GAIN_MIN   1.00f    /* 아주 느릴 때. 1 미만으로 내리면 미세하지만 끊긴다 */
#define GAIN_DIV   70.0f    /* 속도가 붙을수록 제곱으로 는다 (0908: 55→70, 조금 느리게) */
#define GAIN_CAP   3.8f    /* 0908: 4.5→3.8 */
#define VEL_SMOOTH 0.35f    /* 속도 추정의 관성. 크면 민첩하고 작으면 매끈하다 */
#define VEL_IDLE_MS   30    /* 이만큼 입력이 없으면 멈춘 것으로 보고 잦아든다 */
#define VEL_IDLE_DECAY 0.4f

static float      s_vel_x, s_vel_y;      /* 카운트/초 */
static float      s_frac_x, s_frac_y;    /* 아직 못 보낸 소수점 */
static uint32_t   s_last_in_ms;
static lv_timer_t *s_emit;
static int        s_emit_ms = 15;

/* ── 에어마우스 ──────────────────────────────────────────────
 * 배지를 기울여 커서를 움직인다. 손가락으로 문대는 게 아니라 레이저
 * 포인터처럼 겨눈다 — 기울인 '각도'가 커서의 '속도'가 된다(레이트 컨트롤).
 * 공중에서 절대 위치를 잡을 수는 없으니 이게 유일하게 맞는 방식이다.
 *
 * 켤 때의 자세가 한가운데다. 그래서 누워서 들든 팔을 뻗든 상관없다.
 * 켠 상태에서도 탭은 그대로 클릭이다 — 겨누고 눌러야 쓸모가 있다. */
static bool      s_air;
static bool      s_air_default;   /* 홈에서 에어마우스로 들어왔나 */
static lv_obj_t *s_air_btn, *s_air_lbl;
/* 에어마우스일 때 쓰는 화면 — 좌/우 클릭 단추 둘뿐이다.
 * 겨누는 손과 누르는 손이 같으니, 누르는 자리가 커서를 흔들면 안 된다.
 * 그래서 이것들은 커서에 아무 영향을 안 준다.
 *
 * 🚨 예전엔 가운데에 스크롤 전용 세로띠가 있었다. 없앴다(0910 지적) —
 * 화면을 셋으로 갈라 놓으니 문지를 자리가 좁아 거슬렸고, 그 띠가 위쪽
 * 에어/트랙패드 토글을 9px 물고 있었다. 이제 단추가 곧 스크롤 판이다:
 * 톡 치면 클릭, 위아래로 문대면 휠. s_air_bg 는 단추가 안 덮는 가장자리도
 * 문질러지게 깔아둔 투명 판이다. */
static lv_obj_t *s_air_l, *s_air_r, *s_air_bg, *s_disc;
static int32_t   s_scroll_y;
static float     s_scroll_acc;
static float     s_air0x, s_air0y;      /* 켤 때의 자세 */
static bool      s_air0_set;
/* ── 호스트마다 다른 포인터 속도를 여기서 맞춘다 ───────────────
 * 🚨 마우스는 "몇 픽셀 가라" 가 아니라 **몇 카운트 움직였다** 를 보낸다.
 * 카운트를 픽셀로 바꾸는 건 호스트고, 그 비율이 PC 마다 다르다(윈도우의
 * 포인터 속도, 맥의 추적 속도). 그래서 이 배지에서 맞춰둔 감도가 다른 PC
 * 에서 맞을 이유가 없다 — 고칠 수 있는 문제가 아니라 **맞출 수 있게** 만들
 * 문제다. 게이밍 마우스가 기기에 DPI 단추를 다는 것과 같은 이유다.
 *
 * 🚨 이 값은 에어마우스만이 아니라 **트랙패드에도 같이** 걸린다. 호스트가
 * 카운트를 픽셀로 바꾸는 비율은 둘 다에 똑같이 먹으므로, 한쪽만 고치면
 * 둘의 균형이 깨진다. 보내기 직전에 한 번 곱한다.
 *
 * 🚨 기기마다 따로 기억한다. 집 PC 와 회사 PC 의 포인터 속도가 다르면
 * 옮길 때마다 다시 맞춰야 하는데, 그러면 안 쓰게 된다. */
#define SENS_N 5
static const float SENS[SENS_N] = { 0.55f, 0.75f, 1.0f, 1.35f, 1.8f };
#define SENS_DEF 2
static uint8_t   s_sens = SENS_DEF;
static lv_obj_t *s_sens_btn, *s_sens_lbl;

/* NVS 에 기기별로 담는 표. 여덟 대까지 — 본딩이 열다섯이지만 마우스로 쓰는
 * 기기가 그보다 많을 일은 없다. 넘치면 제일 오래된 자리를 쓴다. */
#define SENS_SLOTS 8
typedef struct { uint8_t addr[6]; uint8_t lv; uint8_t used; } sens_row_t;

static void sens_load(void)
{
    uint8_t a[6];
    s_sens = SENS_DEF;
    if (!port_hid_peer_addr(a)) return;        /* 아직 안 붙었다 — 기본값 */
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
    if (!port_hid_peer_addr(a)) return;        /* 누구 것인지 모르면 안 적는다 */
    sens_row_t t[SENS_SLOTS];
    if (!port_kv_read("mousesens", t, sizeof t)) memset(t, 0, sizeof t);
    int slot = -1;
    for (int i = 0; i < SENS_SLOTS; i++)
        if (t[i].used && memcmp(t[i].addr, a, 6) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < SENS_SLOTS; i++) if (!t[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;                    /* 다 찼다 — 맨 앞을 민다 */
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

static bool      s_cal_msg;             /* 영점 잡는 중이라고 알렸나 */
static bool      s_hgrab, s_harmed;     /* 손잡이에서 시작했나 / 홈까지 올렸나 */
static int32_t   s_hy0;
static float     s_gb_x, s_gb_y, s_gb_z; /* 배운 자이로 치우침 (dps) */
/* 중력 방향(센서 틀, 아래쪽이 양수). 느리게 눌러서 뽑는다 — 아래 참고 */
static float     s_dn_x, s_dn_y, s_dn_z;
static bool      s_dn_set;
/* 가로 축(중력에 수직이면서 화면 오른쪽에 가장 가까운 방향). 배지를 옆으로
 * 세워 들면 잠깐 정할 수 없어서, 그럴 땐 직전 것을 그대로 쓴다. */
static float     s_ha_x, s_ha_y, s_ha_z;
static bool      s_ha_set;
static uint16_t  s_gb_n;                /* 처음 재는 동안 모은 표본 수 */
static uint32_t  s_gb_t0;
#define AIR_DEAD   24.0f     /* 이만큼은 흔들려도 안 움직인다 (손떨림) */
/* 🚨 0908 에 0.055 → 0.078 로 올렸는데 여전히 굼떴다(0909 실기).
 * 화면을 가로지르려면 손목을 크게 꺾어야 했다. 두 배로 올리고 상한도 같이
 * 올린다 — 상한만 낮으면 크게 기울여도 거기서 잘려 답답하다. */
/* 🚨 0.078 → 0.160 도 모자랐다. "포인터가 손 움직임을 따라오기 바쁘다"
 * (0909 실기). 한 번 더 올린다. 제곱이라 게인을 두 배 하면 어느 기울기에서든
 * 두 배 빨라진다. 상한도 같이 올려야 크게 꺾었을 때 거기서 안 잘린다. */
/* 0.055 → 0.078 → 0.160 → 0.300 → 0.500. 실기에서 계속 모자랐다(0909). */
#define AIR_GAIN   0.500f    /* 기울기 → 속도 (가로) */
/* 🚨 세로가 가로보다 굼떴다(0909 제보: "좌우는 괜찮은데 위아래를 더 빠르게").
 * 손목은 좌우로 젖히는 각(롤)이 앞뒤로 숙이는 각(피치)보다 넓다. 같은 이득을
 * 주면 세로만 덜 나간다. 축마다 따로 준다. */
#define AIR_GAIN_Y 0.850f    /* 기울기 → 속도 (세로) */
#define AIR_MAX    5000.0f   /* 초당 픽셀 상한 */

/* ── 자이로 ────────────────────────────────────────────────────
 * 🚨 기울기는 '자세' 를 본다. 그래서 배지를 들고 팔을 옮겨도 자세가 그대로면
 * 커서가 안 간다 — 나는 아니까 기울여 쓰지만, 남이 들고 팔을 휘두르면 아무
 * 일도 안 일어난다(0910 지적). 자이로는 '돌아간 만큼' 을 주므로 손목을 돌린
 * 각이 그대로 커서 거리가 된다. TV 리모컨 에어마우스가 이 방식이다.
 *
 * 자이로가 없거나 아직 못 믿을 때는 기울기로 되돌아간다 — 아래 air_drive. */
/* 34 는 조금 빨랐다(0910 실기). 트랙패드는 그대로 두고 이쪽만 내린다. */
#define GYRO_PX_DEG  26.0f   /* 1도 돌리면 몇 픽셀 */
#define GYRO_CURVE   220.0f  /* 이 속도(dps)에서 이득이 두 배 */
#define GYRO_CURVE_CAP 2.4f  /* 이득을 몇 배까지 */
/* 🚨 자이로는 가만히 둬도 0 이 아니다(치우침). 안 빼면 커서가 혼자 흘러간다.
 *
 * 🚨 첫 판에서 실제로 흘렀다(0910 실기: "가만히 있으면 자꾸 오른쪽으로").
 * 원인은 배우는 조건이 스스로를 잠근 것이었다. 첫 값 하나를 치우침으로
 * 삼았는데 그게 켜자마자의 설익은 값이라 몇 dps 어긋났고, "3dps 보다 조용할
 * 때만 배운다" 는 문턱을 그 어긋남이 이미 넘어서, 배우는 코드가 영영 안
 * 돌았다. 문턱으로 잠그는 학습은 이렇게 죽는다.
 *
 * 고친 방식 셋:
 *   1. 켠 뒤 GYRO_CAL_MS 동안은 커서를 안 움직이고 평균만 낸다. 값 하나가
 *      아니라 수십 개의 평균이라 설익은 표본 하나에 안 흔들린다.
 *   2. 그 뒤로는 절대 안 잠긴다. 아주 조용하면 빨리, 어중간하면 아주 천천히,
 *      움직이는 중이면 안 배운다 — 세 단이라 어디서든 빠져나올 길이 있다.
 *   3. 느린 조준(8dps 아래)을 치우침으로 먹지 않게 그 구간은 아주 느리게만
 *      배운다. 34px/도 에서 8dps 는 초당 272px 이라 실제로 쓰는 속도다. */
#define GYRO_CAL_MS  700     /* 켠 직후 이만큼은 재기만 한다 */
#define GYRO_DEAD    1.5f    /* dps. 치우침을 뺀 뒤에도 남는 손떨림 */
#define GYRO_QUIET   2.0f    /* dps. 이보다 조용하면 빨리 배운다 */
#define GYRO_STILL   8.0f    /* dps. 이보다 조용하면 아주 천천히 배운다 */
/* 🚨 실기 로그가 y축 치우침을 -6~-7.4dps 로 찍었다(0910). 34px/도 면 초당
 * 200픽셀이라 눈에 그대로 보인다. 배우긴 하는데 **너무 느렸다** — 손에 들고
 * 있으면 잔여가 10~17dps 로 흔들려서 위의 빠른 단에 거의 안 들어간다.
 * 영점 잡은 직후 얼마간은 문턱을 크게 열어 크게 틀린 것부터 잡는다. */
#define GYRO_WIDE_MS 2500    /* 이 동안은 크게 틀려도 빨리 배운다 */
#define GYRO_WIDE    25.0f   /* 그 동안의 문턱 */
/* 🚨 어떤 상태에서도 도는 아주 느린 새기(시정수 25초). 0 을 주면 문턱 밖에
 * 갇혀 영영 못 빠져나온다 — 첫 판에서 그렇게 당했다. */
#define GYRO_LEAK    0.0006f
#define GYRO_MAXDPS  600.0f
/* 🚨 기울기의 상한(AIR_MAX 5000)을 그대로 쓰면 120dps 에서 이미 잘린다 —
 * 손목 한 번 돌리는 정도다(0910 시뮬에서 확인). 자이로는 '돌린 만큼 간다'
 * 가 전부라 거기서 자르면 관계가 깨진다. 따로 높게 둔다.
 * 진짜 천장은 여기가 아니라 HID 보고다(아래 emit_cb). */
#define GYRO_MAXPX   16000.0f

static float accel_gain(float a)
{
    float g = GAIN_MIN + a * a / GAIN_DIV;
    return g > GAIN_CAP ? GAIN_CAP : g;
}

/* 터치 한 걸음이 들어왔다. 거리가 아니라 속도로 바꿔 담는다. */
static void push_move(float dx, float dy, float dt)
{
    float a = sqrtf(dx * dx + dy * dy);
    float g = accel_gain(a);
    s_vel_x += VEL_SMOOTH * ((dx * g) / dt - s_vel_x);
    s_vel_y += VEL_SMOOTH * ((dy * g) / dt - s_vel_y);
    s_last_in_ms = lv_tick_get();
}

#define IDLE_RELEASE_MS 180000   /* 손 놓고 3분이면 화면을 놓아준다 */

/* 문지른 만큼 휠을 굴린다.
 * 🚨 방향은 마우스를 따른다 — 위로 문대면 위로 올라간다(0910 지적).
 * 손가락을 따라 종이가 끌려오는 터치 감각과는 반대인데, 이건 마우스다. */
static void scroll_feed(int32_t y)
{
    s_scroll_acc += (float)(y - s_scroll_y) * 0.06f;
    s_scroll_y = y;
    while (s_scroll_acc >=  1.0f) { s_scroll_acc -= 1.0f; port_hid_mouse(0, 0, 0,  1); }
    while (s_scroll_acc <= -1.0f) { s_scroll_acc += 1.0f; port_hid_mouse(0, 0, 0, -1); }
}

/* 손가락 하나가 두 일을 한다 — 톡 치면 클릭, 위아래로 문대면 스크롤.
 *
 * 🚨 누르자마자 버튼을 내리면 문지르기가 전부 클릭이 된다. 그래서 내리는 걸
 * 미룬다. 갈림길은 셋이다:
 *   세로로 GES_MIN 넘게 갔다      → 스크롤. 버튼은 끝까지 안 내린다.
 *   안 움직이고 GES_HOLD_MS 넘겼다 → 버튼을 내린다(누른 채 기울여 끄는 용)
 *   그 전에 뗐다                  → 그 자리에서 내렸다 뗀다(빠른 클릭)
 * 한 번 정해지면 그 손가락이 떨어질 때까지 안 바뀐다 — 끌다가 손이 흔들려도
 * 스크롤로 새지 않는다. */
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
                s_scroll_y = p.y;         /* 문턱까지 온 몫은 버린다 — 안 그러면 첫 칸이 튄다 */
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
            /* 문턱도 시간도 안 넘겼다 = 톡 친 것. 놓친 것(PRESS_LOST)은
             * 손가락이 딴 데로 간 것이라 클릭으로 안 친다. */
            port_hid_mouse(0, 0, s_ges_btn, 0);
            port_hid_mouse(0, 0, 0, 0);
        }
        s_ges = GES_NONE;
    }
}

/* 단추가 안 덮는 가장자리. 문지르기만 받는다 — 여기선 클릭이 안 난다. */
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

/* 🚨 에어마우스는 화면을 조준에 안 쓴다 — 손목을 기울여 커서를 민다.
 * 그러니 화면은 통째로 단추여야 한다. 진짜 마우스를 위에서 본 배치로 둔다:
 * 왼쪽 절반이 좌클릭, 오른쪽 절반이 우클릭. 그게 전부다.
 *
 * 🚨 세로가 232px 이라 화면 한가운데서 끝났다(0910 지적: "버튼이 화면
 * 중앙쯤에만 있네"). 위아래로 남길 것은 둘뿐이다 —
 *   위 y<90   에어/트랙패드 토글(y 48..82). 예전엔 스크롤 띠가 이걸 물었다.
 *   아래 y>398 홈으로 가는 손잡이(런처가 y 402..466 에 깐다).
 * 그 사이를 다 쓴다: y 90..396, 세로 306. 모서리 반지름을 56으로 크게 줘서
 * 둥근 화면 밖으로 나가는 귀퉁이를 미리 깎는다. */
static lv_obj_t *mk_click_btn(lv_obj_t *root, int dx, int dy, const char *txt, int right)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, 226, 306);
    lv_obj_set_style_radius(b, 56, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1D1D24), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    /* 🚨 PRESSING 이 있어야 문지르기를 안다. 이걸 안 걸었더니 모든 끌기가
     * 클릭으로 떨어졌다(0910 시뮬에서 잡았다 — 휠이 한 칸도 안 나갔다).
     * PRESS_LOST 도 받는다. 눌린 채로 손가락이 단추 밖으로 나가면 RELEASED 가
     * 안 오는데, 그때 버튼을 안 놓으면 호스트 쪽에 계속 눌린 채로 남는다. */
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
    /* 에어일 땐 트랙패드를 걷고 단추와 스크롤을 낸다. 반대도 마찬가지. */
    #define SHOW(o, on) do { if (o) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); \
                                      else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } } while (0)
    /* 🔋 에어일 때만 자이로를 돌린다. 트랙패드로 돌아가면 곧바로 끈다 —
     * 가속도계보다 열 배 넘게 먹는다. */
    port_imu_gyro_enable(s_air);
    SHOW(s_air_l, s_air);
    SHOW(s_air_r, s_air);
    SHOW(s_air_bg, s_air);
    SHOW(s_ring, !s_air);
    SHOW(s_pad, !s_air);
    SHOW(s_disc, !s_air);      /* 트랙패드 원판은 에어일 때 걷는다 */
    #undef SHOW
    /* 🚨 에어일 땐 화면 가운데가 통째로 단추라 상태 글자가 그 뒤에 깔린다.
     * 단추 아래로 내려 짝짓기 숫자가 가려지지 않게 한다. */
    /* 🚨 단추가 화면을 거의 다 덮어 글자를 비켜 놓을 자리가 없다. 단추 위에
     * 얹는다 — 라벨은 CLICKABLE 이 아니라 터치를 안 먹으므로 그 자리를 눌러도
     * 그대로 클릭이 난다(아래에서 앞으로 끌어낸다). */
    if (s_state) lv_obj_align(s_state, LV_ALIGN_CENTER, 0, s_air ? -100 : -18);
    if (s_hint)  lv_obj_align(s_hint,  LV_ALIGN_CENTER, 0, s_air ?  -74 : 18);
    /* 나가는 손잡이는 에어일 때만 낸다 — 트랙패드는 화면 전체를 쓴다 */
    /* 🚨 트랙패드에도 손잡이를 낸다(0910 요청). 다만 잡는 판은 걷는다 —
     * 화면 전체가 입력이라 아래쪽에서 시작한 문지르기가 죽으면 안 된다.
     * 대신 pad_cb 가 직접 가른다:
     *   손잡이에서 시작 + 위로 올림 → 홈
     *   손잡이에서 시작 + 돌리기    → 그냥 스크롤 (홈 아님)
     *   딴 데서 시작                → 평소대로 */
    launcher_handle_show(true);
    launcher_handle_passthrough(!s_air);
}

static void air_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        /* 다른 기기에 붙이려는데 폰이 자꾸 먼저 낚아챌 때 쓴다 */
        int n = port_hid_forget_all();
        if (s_hint) lv_label_set_text_fmt(s_hint, "forgot %d device%s", n, n == 1 ? "" : "s");
        return;
    }
    s_air = !s_air;
    s_air0_set = false;      /* 켤 때의 자세를 새로 잡는다 */
    s_gb_n = 0;              /* 자이로 영점을 그 자리에서 새로 잡는다 */
    s_gb_x = s_gb_y = s_gb_z = 0;
    s_dn_set = s_ha_set = false;
    s_vel_x = s_vel_y = 0;   /* 남은 속도가 튀지 않게 */
    air_paint();
}

/* 🚨 축마다 따로 판단한다. 예전엔 max(|ax|,|ay|) 하나로 갈랐는데, 그러면
 * 한 축이 시끄러울 때 조용한 축까지 못 배운다 — 로그에서 x 는 잠잠한데 y 가
 * 흔들려 둘 다 멎어 있었다. */
static float learn_k(float a, bool wide)
{
    float m = fabsf(a);
    if (wide && m < GYRO_WIDE) return 0.04f;   /* 켠 직후 — 크게 틀린 걸 먼저 */
    if (m < GYRO_QUIET)        return 0.05f;   /* 확실히 조용하다 */
    if (m < GYRO_STILL)        return 0.004f;  /* 애매 — 조준을 안 먹게 천천히 */
    return GYRO_LEAK;                          /* 움직이는 중에도 아주 조금 */
}

/* 각속도를 그대로 커서 속도로. 돌린 만큼 간다. */
/* 🚨 여태 자이로 x·y 두 축만 썼다. 그래서 **팔을 좌우로 휘두르면 커서가 안
 * 갔다**(0910 제보: "꼭 손목을 기울여야 하던데").
 *
 * 배지를 쟁반처럼 평평하게 들었을 때:
 *   팔을 위아래로 = 화면 평면 안의 축 둘레 회전 → x·y 에 잡힌다 → 됐다
 *   팔을 좌우로   = **중력축 둘레의 요(yaw)** → z 에만 잡힌다 → 안 읽었다
 *   손목을 기울임 = 화면 평면 안의 축 → x·y → 그래서 이것만 됐다
 *
 * 🚨 z 를 가로에 그냥 더하면 안 된다. 배지를 세워 들면 z 가 더 이상 중력축이
 * 아니라 반대로 망가진다. **중력을 기준으로 축을 다시 세워야 한다** —
 * 가속도계가 어느 쪽이 아래인지 알려주니:
 *     요(가로)   = 각속도를 중력 방향에 투영한 성분
 *     피치(세로) = 중력에 수직이면서 화면 오른쪽에 가장 가까운 축에 투영
 * 이러면 어떻게 들든(평평하게든 세워서든, 굴려서 들든) 팔을 좌우로 휘두르면
 * 커서가 좌우로 간다. 진짜 에어마우스가 하는 게 이것이다.
 *
 * 🚨 대신 **손목을 굴려서 가로로 미는 것은 이제 안 된다.** 굴리기(roll)는
 * 겨누는 방향 둘레의 회전이라 어느 쪽도 안 가리킨다 — 그게 맞는 동작이다. */
static void air_drive_tilt(void);   /* 자이로를 못 믿을 때 되돌아간다 */

static void air_drive_gyro(float rx, float ry, float rz)
{
    /* 1) 켠 직후는 재기만 한다. 이 동안 커서는 안 움직인다 — 0.4초짜리
     * 영점 잡기다. 손에 든 채로 켜도 그 자세가 0 이 된다. */
    if (s_gb_n == 0) s_gb_t0 = lv_tick_get();
    if (lv_tick_get() - s_gb_t0 < GYRO_CAL_MS) {
        s_gb_x += (rx - s_gb_x) / (float)(s_gb_n + 1);
        s_gb_y += (ry - s_gb_y) / (float)(s_gb_n + 1);
        s_gb_z += (rz - s_gb_z) / (float)(s_gb_n + 1);
        if (s_gb_n < 60000) s_gb_n++;
        s_vel_x = s_vel_y = 0;
        /* 커서가 안 움직이는 게 고장이 아니라고 알린다 */
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

    /* 2) 그 뒤로는 세 단으로 계속 따라간다. 🚨 어느 단에서도 "못 배우는
     * 상태" 에 갇히지 않는 게 요점이다 — 갇히면 커서가 영영 흐른다.
     * 🚨 움직이는 중엔 안 배운다. 배우면 그 움직임을 0 으로 알아버려서
     * 손을 멈추는 순간 커서가 반대로 튄다.
     * 🚨 z 도 반드시 배워야 한다 — 이제 가로가 z 에 걸리므로, 안 빼면
     *    커서가 옆으로 혼자 흘러간다. */
    bool wide = (lv_tick_get() - s_gb_t0) < (GYRO_CAL_MS + GYRO_WIDE_MS);
    s_gb_x += wx * learn_k(wx, wide);
    s_gb_y += wy * learn_k(wy, wide);
    s_gb_z += wz * learn_k(wz, wide);

    /* 3) 중력이 어느 쪽인지 — 가속도계에서 뽑는다.
     * 🚨 흔들면 가속도계는 중력 말고 팔 가속도까지 읽는다. 그래서 세게 눌러
     * 뽑는다(시정수 약 0.7초). 자세는 천천히 바뀌니 그래도 늦지 않다. */
    float mx = 0, my = 0, mz = 0;
    if (port_imu_accel3(&mx, &my, &mz)) {
        /* 가속도계는 하늘을 향한 축이 양수다 — 아래 방향은 그 반대다. */
        if (!s_dn_set) { s_dn_x = -mx; s_dn_y = -my; s_dn_z = -mz; s_dn_set = true; }
        else {
            s_dn_x += (-mx - s_dn_x) * 0.03f;
            s_dn_y += (-my - s_dn_y) * 0.03f;
            s_dn_z += (-mz - s_dn_z) * 0.03f;
        }
    }
    float dl = sqrtf(s_dn_x * s_dn_x + s_dn_y * s_dn_y + s_dn_z * s_dn_z);
    if (dl < 200.0f) { air_drive_tilt(); return; }   /* 아직 못 믿는다 */
    float gxu = s_dn_x / dl, gyu = s_dn_y / dl, gzu = s_dn_z / dl;

    /* 4) 가로 축 — 화면 오른쪽을 수평면에 눕힌 것.
     * 센서 틀에서 화면 오른쪽은 (0,-1,0) 이다(구슬·물에서 잰 축 그대로).
     * 🚨 배지를 옆으로 세워 오른쪽 모서리가 아래를 보면 이 값이 사라진다.
     *    그럴 땐 직전 축을 그대로 쓴다 — 갑자기 방향이 뒤집히면 안 된다. */
    float rxv = 0.0f, ryv = -1.0f, rzv = 0.0f;
    float rdotg = rxv * gxu + ryv * gyu + rzv * gzu;
    float hx = rxv - rdotg * gxu, hy = ryv - rdotg * gyu, hz = rzv - rdotg * gzu;
    float hl = sqrtf(hx * hx + hy * hy + hz * hz);
    if (hl > 0.30f) {
        s_ha_x = hx / hl; s_ha_y = hy / hl; s_ha_z = hz / hl;
        s_ha_set = true;
    }
    if (!s_ha_set) { air_drive_tilt(); return; }

    /* 5) 각속도를 그 두 축에 투영한다. */
    float yawr   = wx * gxu + wy * gyu + wz * gzu;          /* 좌우로 휘두르기 */
    float pitchr = wx * s_ha_x + wy * s_ha_y + wz * s_ha_z; /* 위아래로 들기 */

    /* 흐르는지 눈으로 못 볼 때를 위해 2초에 한 줄 남긴다. */
    static uint32_t log_ms;
    if (lv_tick_get() - log_ms > 2000) {
        log_ms = lv_tick_get();
        port_log("air", "치우침 %.2f/%.2f/%.2f 요 %.1f 피치 %.1f 아래 %.2f/%.2f/%.2f",
                 (double)s_gb_x, (double)s_gb_y, (double)s_gb_z,
                 (double)yawr, (double)pitchr,
                 (double)gxu, (double)gyu, (double)gzu);
    }

    /* 🚨 죽임과 곡선은 **투영한 값**에 먹인다. 날 축에 먹이면 비스듬히 들었을
     * 때 두 축에 나뉘어 들어가 문턱을 못 넘는다. */
    #define DEAD(v) ((v) > GYRO_DEAD ? (v) - GYRO_DEAD : ((v) < -GYRO_DEAD ? (v) + GYRO_DEAD : 0))
    float ax = DEAD(yawr), ay = DEAD(pitchr);
    #undef DEAD
    if (ax >  GYRO_MAXDPS) ax =  GYRO_MAXDPS;
    if (ax < -GYRO_MAXDPS) ax = -GYRO_MAXDPS;
    if (ay >  GYRO_MAXDPS) ay =  GYRO_MAXDPS;
    if (ay < -GYRO_MAXDPS) ay = -GYRO_MAXDPS;

    /* 천천히 돌리면 촘촘하게, 빨리 돌리면 멀리. 기울기 쪽의 제곱만큼 세지
     * 않게 — 자이로는 이미 속도라 제곱을 먹이면 조준이 안 된다. */
    float sp = sqrtf(ax * ax + ay * ay);
    float g = 1.0f + sp / GYRO_CURVE;
    if (g > GYRO_CURVE_CAP) g = GYRO_CURVE_CAP;
    g *= GYRO_PX_DEG;

    /* 🚨 세로 부호는 예전 것과 정확히 이어진다 — 평평하게 들면 위 피치가
     * -gyro_y 로 떨어지고, 그게 지금까지 쓰던 세로 식이다. 세로는 이미
     * 검증된 값이므로 여기서 다시 뒤집지 않는다.
     * 🚨 가로는 새 축이라 실기 검증이 없다. 팔을 오른쪽으로 휘두르면 각속도가
     *    중력 쪽을 향하므로(위에서 봤을 때 시계방향) 요가 양수 → 커서 오른쪽.
     *    반대로 나오면 이 한 줄의 부호만 뒤집으면 된다. */
    float vx =  ax * g;
    float vy =  ay * g;
    if (vx >  GYRO_MAXPX) vx =  GYRO_MAXPX;
    if (vx < -GYRO_MAXPX) vx = -GYRO_MAXPX;
    if (vy >  GYRO_MAXPX) vy =  GYRO_MAXPX;
    if (vy < -GYRO_MAXPX) vy = -GYRO_MAXPX;

    /* 계단만 지운다. 자이로는 이미 속도라 여기서 더 늦추면 그만큼 지연이다. */
    s_vel_x += (vx - s_vel_x) * 0.9f;
    s_vel_y += (vy - s_vel_y) * 0.9f;

    if (vx != 0 || vy != 0) s_last_in_ms = lv_tick_get();
}

/* 기울기를 커서 속도로. 켤 때의 자세에서 얼마나 벗어났나만 본다.
 * 자이로가 없는 기기(시뮬 포함)와 자이로가 아직 안 깬 동안을 받는다. */
static void air_drive_tilt(void)
{
    /* 🚨 IMU 를 따로 타이머로 묶어 읽던 것이 마지막 지연원이었다.
     * 20ms → 10ms 로 당겨도 값이 최대 10ms 묵는다. 그런데 이 함수는
     * **송신할 때 딱 한 번** 불린다(emit_cb). 그러면 그 자리에서 바로 읽는
     * 것이 맞다 — 묵은 값이 아예 없어지고, I2C 도 오히려 준다
     * (10ms 마다 = 초당 100회 → 송신마다 = 초당 66회). 0909 */
    float gx, gy;
    if (!port_imu_accel(&gx, &gy)) return;
    if (!s_air0_set) { s_air0x = gx; s_air0y = gy; s_air0_set = true; }
    float dx = gx - s_air0x, dy = gy - s_air0y;

    /* 🚨 IMU 축은 화면 축과 90도 돌아가 있다 — gx 가 세로(앞뒤),
     * gy 가 가로(좌우)를 맡는다. 구슬에서 확인한 그대로다.
     * 우로 기울이면 gy 가 음수이므로 부호를 뒤집어야 커서가 오른쪽으로 간다.
     * 🚨 세로는 뒤집혀 있었다 — 앞으로 숙이면 커서가 위로 갔다(0909 실기).
     * 가로(-dy)는 맞고 세로만 부호가 반대였다. */
    float ax = -dy, ay = dx;

    /* 가만히 들고 있어도 손은 떨린다. 그만큼은 죽인다. */
    #define DEAD(v) ((v) > AIR_DEAD ? (v) - AIR_DEAD : ((v) < -AIR_DEAD ? (v) + AIR_DEAD : 0))
    ax = DEAD(ax); ay = DEAD(ay);
    #undef DEAD

    /* 조금 기울이면 천천히, 많이 기울이면 확 — 제곱으로 준다.
     * 그래야 미세 조준과 화면 가로지르기가 한 손에 들어온다. */
    float vx = ax * fabsf(ax) * AIR_GAIN   * 0.01f;
    float vy = ay * fabsf(ay) * AIR_GAIN_Y * 0.01f;
    if (vx >  AIR_MAX) vx =  AIR_MAX;
    if (vx < -AIR_MAX) vx = -AIR_MAX;
    if (vy >  AIR_MAX) vy =  AIR_MAX;
    if (vy < -AIR_MAX) vy = -AIR_MAX;

    /* 곧바로 바꾸면 떨림이 그대로 간다. 조금 늦게 붙인다.
     * 🚨 0.35 는 송신 주기 15ms 기준 시정수 43ms 라 눈에 띄게 늦었다.
     * 0.55 로도 모자라 0.75 까지 올린다(시정수 약 5ms). 떨림은 위의 죽임
     * 구간이 이미 잡으므로 여기서 두 번 누를 이유가 없다 — 이 평활은
     * 떨림 제거가 아니라 계단 지우기 용도로만 남긴다. */
    /* 🚨 0.75(시정수 약 11ms)도 아직 느끼는 사람이 있다. 0.88 이면 약 4ms 다.
     * 떨림은 위의 죽임 구간이 이미 잡으므로 여기서 두 번 누를 이유가 없다. */
    s_vel_x += (vx - s_vel_x) * 0.88f;
    s_vel_y += (vy - s_vel_y) * 0.88f;

    /* 🚨 에어마우스는 화면을 안 만진다. 그대로 두면 무동작으로 판단해
     * 화면이 꺼지고, 꺼지면 송신이 500ms 로 늦춰져 커서가 멎는다.
     * 기울여 움직이는 것도 조작이다 — 움직이는 동안만 깨워둔다.
     * 가만히 들고 있으면 평소처럼 꺼진다. */
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
    /* 🔋 화면이 꺼지면 아무도 안 본다. 다만 🚨 여기서 주기를 바꾸면 안 된다 —
     * lv_timer_set_period() 는 안쪽에서 lv_timer_handler_resume() 을 불러서,
     * 타이머 콜백에서 부르면 처리기가 그 자리에서 무한히 다시 돈다.
     * (0909: 절전하려고 넣었다가 CPU 를 100% 물고 늘어지게 만들었다.
     *  값이 같아도 마찬가지라 "바뀔 때만 세우기"로도 못 막는다.)
     * 주기는 그대로 두고 33번에 한 번만 일한다. 효과는 같고 안전하다. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 33) return;
    }
   /* 연결 간격에 맞춘 값으로 되돌린다 */
    /* 🚨 예전엔 .keep_awake = true 가 상한 없이 걸려서, 마우스 앱에 들어간 채
     * 두면 화면이 영원히 켜져 있었다(켬/끔 차이가 시간당 124mV 다).
     * 쓰는 동안만 붙잡고 손을 놓으면 놓아준다. 다시 만지면 바로 켜진다. */
    launcher_keep_awake(lv_tick_get() - s_last_in_ms < IDLE_RELEASE_MS);
    float dt = s_emit_ms / 1000.0f;

    if (s_air) air_drive();

    /* 손가락이 멎었는데 속도가 남아 있으면 커서가 미끄러진다.
     * 에어마우스일 땐 기울기가 계속 몰고 있으니 건너뛴다. */
    if (!s_air && lv_tick_get() - s_last_in_ms > VEL_IDLE_MS) {
        s_vel_x *= VEL_IDLE_DECAY;
        s_vel_y *= VEL_IDLE_DECAY;
        if (fabsf(s_vel_x) < 1.0f) s_vel_x = 0;
        if (fabsf(s_vel_y) < 1.0f) s_vel_y = 0;
    }

    /* 🚨 여기서 한 번만 곱한다. 기울기·자이로·트랙패드가 다 이 자리를
     * 지나므로 셋의 균형이 안 깨진다. */
    float k = SENS[s_sens < SENS_N ? s_sens : SENS_DEF];
    float wx = s_vel_x * dt * k + s_frac_x;
    float wy = s_vel_y * dt * k + s_frac_y;
    int ix = (int)wx, iy = (int)wy;
    /* 🚨 HID 보고는 한 번에 ±127 픽셀이 천장이다(8비트 상대좌표). 넘는 몫을
     * 버리면 확 돌렸을 때 그만큼 덜 간다 — 자이로는 "돌린 만큼 간다" 가
     * 전부라 그러면 관계가 깨진다. 잘라내고 남는 건 다음 보고로 넘긴다. */
    if (ix >  127) ix =  127;
    if (ix < -127) ix = -127;
    if (iy >  127) iy =  127;
    if (iy < -127) iy = -127;
    s_frac_x = wx - (float)ix;      /* 못 보낸 소수점과 남은 몫은 다음으로 */
    s_frac_y = wy - (float)iy;
    /* 🚨 다만 밀린 몫이 끝없이 쌓이면 손을 멈춘 뒤에도 커서가 계속 미끄러진다.
     * 서너 보고 안에 갚을 만큼만 들고 있는다. */
    /* 🚨 400px 은 너무 넉넉했다 — 확 돌린 뒤 서너 보고 동안 커서가 계속
     * 미끄러진다. 두 보고 안에 갚을 만큼으로 줄인다. */
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

/* ── 길게 누르면 우클릭 ─────────────────────────────────────── */

static void hold_cb(lv_timer_t *t)
{
    (void)t;
    s_hold = NULL;
    if (!s_moved) {
        s_moved = true;            /* 뗄 때 좌클릭이 또 나가지 않게 */
        click(2);
        lv_label_set_text(s_hint, "right click");
    }
}

static void cancel_hold(void)
{
    if (s_hold) { lv_timer_delete(s_hold); s_hold = NULL; }
}

/* ── 손가락 ─────────────────────────────────────────────────── */

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
        /* 손잡이에서 시작했나. 시작만 기억해두고 하던 일은 그대로 한다 —
         * 위로 올리지 않으면 평소와 똑같이 굴러야 하니까. */
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
        s_tilt_lock = s_tilt_deg;      /* 이번 터치 동안은 이 각으로 고정 */
        s_two = false;
        s_two_acc = 0;
        s_mode = (r >= RING_R_IN) ? MODE_RING : MODE_PAD;
        s_last_ang = atan2f((float)ry, (float)rx) * 57.2958f;
        cancel_hold();
        if (s_drag_lock) {
            /* 버튼을 누른 채로 시작한다. 뗄 때까지 눌려 있다. */
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

        /* 두 번째 손가락이 닿았나. 한 번 닿으면 이번 터치 내내 기억한다 —
         * 뗄 때 둘 다 붙어 있으란 법이 없다. */
        if (port_touch_count() >= 2) {
            if (!s_two) { s_two = true; cancel_hold(); }
            s_mode = MODE_TWO;
        }

        if (s_mode == MODE_TWO) {
            /* 트랙패드처럼 두 손가락으로 끌면 스크롤 */
            s_two_acc += dy;
            while (s_two_acc >= TWO_SCROLL_PX)  { s_two_acc -= TWO_SCROLL_PX; port_hid_mouse(0, 0, 0, -1); s_moved = true; }
            while (s_two_acc <= -TWO_SCROLL_PX) { s_two_acc += TWO_SCROLL_PX; port_hid_mouse(0, 0, 0,  1); s_moved = true; }
            if (s_moved) lv_label_set_text(s_hint, "two-finger scroll");
        } else if (s_mode == MODE_RING) {
            float ang = atan2f((float)ry, (float)rx) * 57.2958f;
            float d = ang - s_last_ang;
            while (d > 180)  d -= 360;      /* 12시를 넘어갈 때 튀는 걸 막는다 */
            while (d < -180) d += 360;
            s_last_ang = ang;
            s_ring_acc += d;
            while (s_ring_acc >= WHEEL_DEG)  { s_ring_acc -= WHEEL_DEG; port_hid_mouse(0, 0, 0, -1); }
            while (s_ring_acc <= -WHEEL_DEG) { s_ring_acc += WHEEL_DEG; port_hid_mouse(0, 0, 0,  1); }
            if (fabsf(d) > 0.5f) { s_moved = true; cancel_hold(); }
            lv_label_set_text(s_hint, "scroll");
        } else {
            if (abs(dx) > MOVE_SLOP || abs(dy) > MOVE_SLOP) { s_moved = true; cancel_hold(); }
            /* 좌표를 먼저 눌러서 눈금 잡음을 뺀다. 델타는 필터를 통과한
             * 값끼리 뺀 것이라 1픽셀씩 튀던 게 사라진다. */
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
                /* 🚨 launcher_home() 은 이 앱을 닫는다. 뒤에서 화면 조각을
                 * 만지면 이미 없는 자리를 밟는다 — 정리하고 곧장 나간다. */
                cancel_hold();
                motion_reset();
                port_hid_mouse(0, 0, 0, 0);
                s_mode = MODE_NONE;
                launcher_home();
                return;
            }
        }
        cancel_hold();
        /* 손을 뗐으면 속도를 즉시 죽인다. 안 그러면 커서가 미끄러진다. */
        motion_reset();
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        s_last_release_ms = lv_tick_get();
        s_last_release_pt = p;

        if (s_drag_lock) {
            port_hid_mouse(0, 0, 0, 0);      /* 버튼 놓기 */
            s_drag_lock = false;
            lv_label_set_text(s_hint, "2 fingers = right click");
            s_mode = MODE_NONE;
            return;
        }
        if (!s_moved && s_two) {
            /* 두 손가락 탭 = 우클릭. 트랙패드 관례 그대로다.
             * 길게 누르기도 그대로 남겨둔다 — 엄지로 쥐면 손가락 둘을 못 쓴다. */
            click(2);
            lv_label_set_text(s_hint, "right click");
        } else if (!s_moved && s_mode == MODE_PAD) {
            click(1);
            lv_label_set_text(s_hint, "click");
        }
        s_mode = MODE_NONE;
    }
}

/* ── 연결 상태 ──────────────────────────────────────────────── */

/* 🚨 "값이 바뀔 때만 그린다" 를 쓰면 화면을 새로 지을 때 반드시 한 번은
 * 써야 한다. 안 그러면 LVGL 이 라벨에 넣어두는 기본 글자 Text 가 그대로
 * 남는다 — 배터리 숫자에서 겪은 것과 같은 덫이고, 여기선 BLE 가 안 뜨거나
 * 상태가 지난번과 같으면 트랙패드에 "Text" 가 떴다(0908 지적). */
static int      s_prev_conn = -1;
static uint32_t s_prev_key  = 0xFFFFFFFF;
static char     s_prev_peer[24];

static void poll_cb(lv_timer_t *t)
{

    /* 🔋 화면이 꺼지면 아무도 안 본다. 다만 🚨 여기서 주기를 바꾸면 안 된다 —
     * lv_timer_set_period() 는 안쪽에서 lv_timer_handler_resume() 을 불러서,
     * 타이머 콜백에서 부르면 처리기가 그 자리에서 무한히 다시 돈다.
     * (0909: 절전하려고 넣었다가 CPU 를 100% 물고 늘어지게 만들었다.
     *  값이 같아도 마찬가지라 "바뀔 때만 세우기"로도 못 막는다.)
     * 주기는 그대로 두고 5번에 한 번만 일한다. 효과는 같고 안전하다. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 5) return;
    }

    /* LVGL 은 값이 같아도 스타일을 세우면 무조건 다시 그린다. 452px 링을
     * 초당 2.5번 헛되이 무효화하고 있었다 — 바뀔 때만 손댄다. */
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
        /* 폰에 뜬 숫자와 같은지 눈으로 맞추라고 크게 띄운다 */
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
    /* 가장자리 링 = 스크롤 구역이자 연결 상태 표시 */
    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, 452, 452);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);

    /* 트랙패드 판 */
    s_disc = lv_obj_create(root);
    lv_obj_remove_style_all(s_disc);
    lv_obj_set_size(s_disc, PAD_R * 2, PAD_R * 2);
    lv_obj_center(s_disc);
    lv_obj_set_style_radius(s_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_disc, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(s_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_disc, 1, 0);
    lv_obj_set_style_border_color(s_disc, lv_color_hex(0x26262C), 0);

    s_prev_conn = -1;                     /* 다음 갱신이 반드시 쓰게 */
    s_prev_key  = 0xFFFFFFFF;
    s_prev_peer[0] = '\0';

    /* 🚨 쓰던 중에 다른 PC 로 옮기고 싶을 때 설정까지 들어가는 건 멀다.
     * 지금 붙은 상대를 적는 그 글자를 누르면 목록이 뜬다 — 자리가 뜻과 맞다. */
    s_state = lv_label_create(root);
    lv_label_set_text(s_state, "connecting");   /* 기본 글자 Text 를 덮는다 */
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x5E5E66), 0);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_20, 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, -18);

    s_hint = lv_label_create(root);
    lv_label_set_text(s_hint, "tap twice, then drag");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5E5E66), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 18);

    /* 에어마우스 켜고 끄기 */
    s_air = s_air_default;
    s_air0_set = false;
    s_air_btn = lv_button_create(root);
    lv_obj_set_size(s_air_btn, 64, 34);
    lv_obj_set_style_radius(s_air_btn, 17, 0);
    lv_obj_set_style_shadow_width(s_air_btn, 0, 0);
    /* 🚨 감도 단추와 나란히 앉는다. 둘 다 64px 이라 ∓40 이면 사이가 16px 뜬다.
     * 제일 먼 모서리가 (96,-185) = 중심에서 208px 이라 반지름 233 안이다. */
    lv_obj_align(s_air_btn, LV_ALIGN_CENTER, -40, -168);
    lv_obj_add_event_cb(s_air_btn, air_cb, LV_EVENT_CLICKED, NULL);
    /* 길게 = 짝지은 기기 전부 잊기 */
    lv_obj_add_event_cb(s_air_btn, air_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_air_lbl = lv_label_create(s_air_btn);
    lv_label_set_text(s_air_lbl, LV_SYMBOL_GPS);
    lv_obj_center(s_air_lbl);
    /* 감도 — 새 PC 에 붙였을 때 두어 번 눌러 맞춘다. 그 기기 것으로 기억한다. */
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
    sens_load();          /* 붙어 있는 기기 것을 꺼내 온다 */
    sens_paint();

    /* 🚨 여기서 air_paint() 를 부르면 안 된다 — 좌·우 단추와 스크롤이 아직
     * 안 만들어져서 숨은 채로 남는다. 다 만든 뒤 아래에서 한 번만 부른다
     * (0909: 홈에서 에어로 바로 들어가면 단추가 아예 안 보였다). */

    /* ── 에어마우스 화면 ────────────────────────────────────
     * 기울여 겨누고, 좌우 반쪽으로 누르고, 아무 데나 문대 스크롤한다.
     *
     * 이 투명 판은 단추가 안 덮는 가장자리(위 띠, 아래 띠, 가운데 틈)를
     * 받는다. 단추보다 뒤에 있어야 하고 토글·손잡이보다도 뒤여야 한다 —
     * 토글은 아래에서 앞으로 끌어내고, 손잡이는 런처가 다른 판에 깐다. */
    s_air_bg = lv_obj_create(root);
    lv_obj_remove_style_all(s_air_bg);
    lv_obj_set_size(s_air_bg, 466, 466);
    lv_obj_center(s_air_bg);
    lv_obj_add_flag(s_air_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_air_bg, air_bg_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_air_bg, air_bg_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_flag(s_air_bg, LV_OBJ_FLAG_HIDDEN);

    /* 화면을 좌우로 반씩. 사이는 6px 만 띄운다 — 손끝이 경계를 헷갈릴 일은
     * 없다(양쪽 다 눌러도 되는 자리다). 가운데 y 는 243 = dy +10. */
    s_air_l = mk_click_btn(root, -117, 10, "L", 0);
    s_air_r = mk_click_btn(root,  117, 10, "R", 1);

    /* 손가락 자리 표시 — 화면을 가려도 어디를 눌렀는지 보인다 */
    s_dot = lv_obj_create(root);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 28, 28);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x7FB0FF), 0);
    lv_obj_set_style_bg_opa(s_dot, 70, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    /* 화면 전체가 입력을 받는다. 링은 좌표로 갈라낸다. */
    s_pad = lv_obj_create(root);
    lv_obj_remove_style_all(s_pad);
    lv_obj_set_size(s_pad, 466, 466);
    lv_obj_center(s_pad);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_RELEASED, NULL);

    /* 🚨 s_pad 가 화면 전체를 덮는 데다 나중에 만들어져서, 그냥 두면
     * 위에 있는 단추들이 눌리지 않는다(0909: 에어 버튼이 안 눌렸다).
     * 만든 순서가 곧 앞뒤라 명시적으로 앞으로 끌어낸다. */
    lv_obj_move_foreground(s_air_bg);
    lv_obj_move_foreground(s_air_l);
    lv_obj_move_foreground(s_air_r);
    /* 🚨 토글이 제일 위여야 한다. 단추가 화면 위끝까지 올라와서, 뒤에 두면
     * 에어에서 트랙패드로 못 돌아온다(0910 지적: 단추가 토글을 물고 있었다). */
    lv_obj_move_foreground(s_air_btn);
    lv_obj_move_foreground(s_sens_btn);
    /* 글자는 단추 위에 얹는다. 라벨은 터치를 안 먹으니 눌림엔 영향이 없다. */
    lv_obj_move_foreground(s_state);
    lv_obj_move_foreground(s_hint);

    air_paint();          /* 다 만든 뒤에 모드에 맞춰 보이고 감춘다 */

    /* 송신 주기는 폰이 허락한 연결 간격에 맞춘다. 그보다 자주 쏴봐야
     * 스택 큐에만 쌓이고 지연이 는다. 대개 7.5~15ms 사이로 잡힌다. */
    int iv = port_hid_interval_ms();
    if (iv < 6) iv = 6;
    s_emit_ms = iv;
    s_emit = lv_timer_create(emit_cb, iv, NULL);
    port_log("mouse", "송신 주기 %dms (연결 간격 %dms)", iv, port_hid_interval_ms());

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
    port_hid_mouse(0, 0, 0, 0);      /* 버튼이 눌린 채로 나가지 않게 */
    /* 🔋 🚨 나갈 때 반드시 끈다. 안 끄면 앱을 닫아도, 화면을 꺼도 자이로가
     * 계속 돈다 — 가속도계에서 똑같이 당했다(그건 5초 뒤 재우기로 막았다). */
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
    /* 들어갈 땐 켠 채로 시작하고, 손 놓고 3분이면 emit_cb 가 놓아준다. */
    .radio = RADIO_BLE, .keep_awake = true, .enter = enter, .leave = leave,
};

/* 홈에서 바로 에어마우스로 들어가는 문. 앱은 같고 시작 모드만 다르다. */
static void enter_air(lv_obj_t *root) { s_air_default = true; enter(root); s_air_default = false; }

const badge_app_t app_air = {
    .name = "Air Mouse", .art = &app_icon_mouse, .icon = LV_SYMBOL_GPS, .tint = tint,
    /* 들어갈 땐 켠 채로 시작하고, 손 놓고 3분이면 emit_cb 가 놓아준다. */
    .radio = RADIO_BLE, .keep_awake = true, .enter = enter_air, .leave = leave,
};
