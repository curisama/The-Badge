#include "app.h"
#include "port.h"
#include "display.h"
#include "assets/assets.h"
#include <math.h>
#include <math.h>

static const char *TAG = "launcher";

/* 화면 지름. 원형이라 모서리에 UI를 두면 잘린다. */
#define SCREEN_D        466
/* 앱이 늘면 타일이 겹친다. 개수에 맞춰 줄인다. */
#define RING_R          (APP_CNT <= 4 ? 122 : (APP_CNT <= 5 ? 132 : 150))
#define ICON_D          (APP_CNT <= 4 ? 126 : (APP_CNT <= 5 ? 120 : 92))

static lv_obj_t          *s_home;
static lv_obj_t          *s_batt;
static int                s_prev_p = -2;   /* 배터리 표시 중복 방지 — 홈을 새로 지으면 되돌린다 */
static bool               s_prev_plug;
static lv_obj_t          *s_app_scr;
static const badge_app_t *s_current;

/* ── 화면 끄기 ────────────────────────────────────────────────
 * AMOLED + 작은 배터리라 이게 없으면 몇 시간이면 빈다.
 * 깨우는 탭은 삼켜야 한다 — 안 그러면 켜면서 아이콘이 눌린다. */
static lv_obj_t   *s_veil;          /* 화면 끈 동안 덮는 검은 판 */
static int         s_crumb_before_off = -1;
static bool        s_touch_wakes;   /* 터치로 깨울 수 있나 */
/* 앱이 "지금은 끄지 마라"고 잠깐 붙잡는다 (타이머 카운트다운 등) */
static bool        s_awake_hold;

/* 🚨 여럿이 동시에 "꺼지지 마" 라고 할 수 있다 — 스톱워치가 도는 중에
 * 타이머 알람이 울리는 식이다. 예전엔 한 놈이 끄면 다른 놈 것까지 같이
 * 꺼졌다. 누가 잡고 있는지 따로 세어, 다 놓아야 풀린다. */
static uint8_t s_awake_bits;
void launcher_keep_awake_by(int who, bool on)
{
    if (who < 0 || who > 7) return;
    if (on) s_awake_bits |= (uint8_t)(1u << who);
    else    s_awake_bits &= (uint8_t)~(1u << who);
    s_awake_hold = (s_awake_bits != 0);
}
void launcher_keep_awake(bool on) { launcher_keep_awake_by(AWAKE_APP, on); }
static lv_timer_t *s_idle;
static lv_timer_t *s_rotate_timer, *s_heap_timer, *s_batt_timer;
static bool  s_autorotate = false;

static int         s_timeout_s = 30;

static void screen_wake(void);

/* ── 화면 꺼짐 절전 ───────────────────────────────────────────
 * 화면이 꺼져 있어도 CPU 는 계속 깨어난다. 제일 잦은 게 터치 읽기로,
 * 12ms 마다 = 초당 83번 I2C 를 때린다. 그런데 이 물건은 자동으로 꺼졌든
 * 손으로 껐든 **터치로는 안 깨운다**(주머니 오작동 방지, idle_cb 참고).
 * 즉 꺼진 동안 터치를 읽을 이유가 아예 없다 — 멈춘다.
 * 라이트슬립처럼 위험한 물건이 아니다. 타이머를 세우고 되돌리는 것뿐이라
 * 최악이라도 "깨어날 때 터치가 한 박자 늦는" 정도다. */
static void idle_timers(bool screen_on)
{
    /* 🚨 제일 큰 것: LVGL 새로고침 타이머. 그릴 게 없어도 33ms 마다 돈다
     * (초당 30번). 화면이 꺼져 있으면 그릴 이유가 없는데도 계속 깨워서,
     * 라이트슬립이 들어가고 나오는 비용만 치르고 제대로 자질 못한다.
     * 이걸 세우면 남는 최장 간격이 200ms(버튼) 가 되어 깊이 잔다. */
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_timer_t *rt = lv_display_get_refr_timer(d);
        if (rt) { if (screen_on) lv_timer_resume(rt); else lv_timer_pause(rt); }
    }
    /* 무동작 검사도 꺼진 동안엔 의미가 없다 — 이미 꺼져 있다. */
    if (s_idle) { if (screen_on) lv_timer_resume(s_idle); else lv_timer_pause(s_idle); }

    lv_indev_t *in = badge_display_indev();
    if (in) {
        lv_timer_t *rt = lv_indev_get_read_timer(in);
        if (rt) {
            if (screen_on) { lv_timer_resume(rt); lv_timer_set_period(rt, 12); }
            else             lv_timer_pause(rt);
        }
    }
    if (s_rotate_timer) {
        /* 자동회전은 기본 꺼짐이다. 꺼져 있으면 200ms 마다 깨울 이유가 없다. */
        if (screen_on && s_autorotate) lv_timer_resume(s_rotate_timer);
        else                           lv_timer_pause(s_rotate_timer);
    }
    if (s_heap_timer)  lv_timer_set_period(s_heap_timer,  screen_on ? 10000 : 120000);
    if (s_batt_timer) {
        /* 안 보이는 라벨을 20초마다 새로 칠할 이유가 없다 */
        if (screen_on) lv_timer_resume(s_batt_timer);
        else           lv_timer_pause(s_batt_timer);
    }
}

static void wake_cb(lv_event_t *e)
{
    (void)e;
    screen_wake();
}

static void screen_off(bool touch_wakes)
{
    if (s_veil) return;
    /* 🚨 끄기 '직전' 에 일지를 한 줄 남긴다. 이 줄과 켤 때 남긴 줄이 이웃해서,
     * 그 사이가 "화면을 켜 둔 구간" 이 된다. 끄고 나서 남기면 화면X 로 적혀
     * 짝이 깨진다 — 여태 화면 켠 소모를 한 번도 못 잰 게 이것 때문이다. */
    port_battery_mark(true);
    s_touch_wakes = touch_wakes;
    /* 🚨 끄기 직전에 무엇을 세고 있었는지 붙잡아 둔다. 잠금을 풀고 그 앱으로
     * 돌아갈 때 앱별 소모 집계를 이어 붙여야 한다 — 안 그러면 그 구간이
     * 통째로 '잠금' 으로 잡힌다. */
    s_crumb_before_off = port_crumb_now();
    port_crumb(CRUMB_SCR_OFF);
    port_log(TAG, "screen OFF (touch_wakes=%d)", touch_wakes);
    if (!touch_wakes) idle_timers(false);   /* 터치로 안 깨우면 읽을 이유가 없다 */
    lock_stop();
    port_display_power(false);
    /* 최상위 층에 덮으면 밑에 뭐가 있든 손가락이 안 닿는다 */
    s_veil = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_veil);
    lv_obj_set_size(s_veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_veil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_veil, LV_OPA_COVER, 0);
    /* 손으로 끈 경우엔 터치를 안 받는다. 주머니에 넣으면 스치기만 해도
     * 켜져서 배터리가 녹는다 — 폰 잠금 버튼과 같은 규칙이다.
     * 어느 쪽이든 덮개 자체는 밑을 막아준다. */
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);
    if (touch_wakes) lv_obj_add_event_cb(s_veil, wake_cb, LV_EVENT_PRESSED, NULL);
}

void launcher_screen_off(void)        { screen_off(true);  }
void launcher_screen_off_manual(void) { screen_off(false); }

bool launcher_screen_is_off(void) { return s_veil != NULL; }

static void screen_wake(void)
{
    if (!s_veil) return;
    /* 🚨 여기가 "화면 껐다 켜면 재부팅"의 현장이다. 깨우기 직전에 남긴다 —
     * 재부팅 뒤 기록에 '화면켬'이 찍혀 있으면 이 경로가 범인이다. */
    /* 여기선 찍지 않는다. 곧바로 잠금화면이나 앱이 자기 이름을 찍는다 —
     * 여기서 '화면켬'을 남기면 그게 그대로 굳어 앱별 집계가 망가진다. */
    idle_timers(true);
    lv_obj_delete(s_veil);
    s_veil = NULL;
    port_display_power(true);
    port_log(TAG, "screen ON");
    /* 🚨 예전엔 앱 안이면 그 앱으로 곧장 돌아갔다. 홈에서 깨면 잠금화면인데
     * 앱에서 깨면 앱이라 규칙이 둘이었다(0910 지적). 언제나 잠금화면으로
     * 통일한다 — 시계처럼 쓰는 물건이고, 주머니 안에서 실수로 앱을 건드리는
     * 일도 준다. 앱은 지워지지 않고 뒤에 그대로 살아 있어서, 손잡이를
     * 올리면 하던 자리로 돌아간다(lock.c 의 launcher_unlock). */
    launcher_show_lock();
    /* 깨웠으면 무동작 시계도 되돌려야 한다. 안 그러면 다음 검사(1초 뒤)가
     * "아직 30초 지난 상태"로 보고 곧바로 다시 꺼버린다. */
    lv_display_trigger_activity(NULL);
    /* 화면 켠 구간의 시작점. 🚨 화면을 켠 뒤에 부른다 — I2C 로 전압을 읽는
     * 동안 화면이 늦게 켜지면 그게 더 눈에 띈다. */
    port_battery_mark(true);
}

/* PWR 을 길게 누르면 AXP2101 이 몇 초 뒤 전원을 끊는다. 우리가 막을 수는
 * 없지만, 눌린 게 먹혔다는 건 보여줘야 한다. 안 그러면 고장인 줄 안다. */
void launcher_poweroff_notice(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, "power off");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9A9A9E), 0);
    lv_obj_center(l);
}

/* 홈은 은하수 배경이라 켜진 화소가 많다. 깨어날 때는 잠금화면(검정)으로 간다. */
void launcher_show_home(void)
{
    /* 일지가 "지금 뭘 보고 있나"로 앱별 소모를 가른다. 여기서 안 찍으면
     * 잠금해제 뒤에도 '화면켬'으로 남아 홈 구간이 통째로 잘못 잡힌다. */
    port_crumb(CRUMB_HOME);
    lock_stop();
    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

/* 잠금을 푼다. 🚨 홈으로 보내면 하던 일이 끊긴다 — 스톱워치가 돌고 있거나
 * 마우스를 쓰던 중일 수 있다. 앱이 살아 있으면 그리로 돌려보낸다. */
void launcher_unlock(void)
{
    if (s_app_scr && lv_obj_is_valid(s_app_scr)) {
        if (s_crumb_before_off >= 0) port_crumb(s_crumb_before_off);
        lock_stop();
        lv_screen_load_anim(s_app_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
        return;
    }
    launcher_show_home();
}

void launcher_show_lock(void)
{
    port_crumb(CRUMB_LOCK);
    lv_obj_t *lk = lock_screen();
    lock_start();
    lv_screen_load_anim(lk, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

/* 알람처럼 "지금 사람이 봐야 하는 일"이 생겼을 때 화면을 켠다.
 * keep_awake 는 꺼지는 걸 막을 뿐, 이미 꺼진 화면을 켜지는 못한다. */
void launcher_screen_on(void)
{
    if (s_veil) screen_wake();
}

void launcher_screen_toggle(void)
{
    if (s_veil) screen_wake();
    else        launcher_screen_off_manual();
}

static void idle_cb(lv_timer_t *t)
{
    (void)t;
    /* 🚨 알람은 앱 밖에 산다. 시계 앱을 닫아도, 화면이 꺼져 있어도 울려야
     * 하기 때문이다. 그래서 앱이 아니라 여기서 매초 확인한다.
     * 아래 이른 반환들보다 **앞**에 있어야 한다 — 화면이 꺼져 있으면
     * (s_veil) 저 아래로 못 내려간다. */
    alarm_tick();
    if (s_timeout_s <= 0 || s_veil) return;
    /* 마우스는 화면이 꺼지면 덮개가 터치를 막아 조작 자체가 죽는다.
     * 다른 화면을 보며 쓰는 물건이라 무동작 시간도 의미가 없다. */
    if (s_current && s_current->keep_awake) return;
    if (s_awake_hold) return;
    if (lv_display_get_inactive_time(NULL) > (uint32_t)s_timeout_s * 1000) {
        /* 자동으로 꺼진 것도 터치로는 안 깨운다. 주머니에서 스치기만 해도
         * 켜지면 배터리가 녹는다. 깨우는 건 버튼이다. */
        launcher_screen_off_manual();
    }
}

void launcher_set_timeout(int seconds) { s_timeout_s = seconds; }
int  launcher_get_timeout(void)        { return s_timeout_s; }

/* 홈을 두 쪽으로 나눈다. 한 쪽에 여섯을 욱여넣으면 아이콘이 92px 로 작아져
 * 누르기 힘들다 — 나누면 120px 로 커진다. 넘길 때만 다시 그리므로
 * 가만있을 때 드는 값은 없다. */
static const badge_app_t *const s_page0[] = { &app_games, &app_orb, &app_water,
                                              &app_air, &app_clock, &app_calc };
static const badge_app_t *const s_page1[] = { &app_mouse, &app_keys, &app_meet };
#define PAGE_N 2
static const badge_app_t *const *const s_pages[PAGE_N] = { s_page0, s_page1 };
static const uint8_t s_page_cnt[PAGE_N] = {
    (uint8_t)(sizeof(s_page0) / sizeof(s_page0[0])),
    (uint8_t)(sizeof(s_page1) / sizeof(s_page1[0])),
};
static uint8_t s_page;          /* 지금 보고 있는 쪽 */

#define s_apps   (s_pages[s_page])
#define APP_CNT  ((size_t)s_page_cnt[s_page])

static lv_timer_t *s_ble_off_timer;

static void ble_off_cb(lv_timer_t *t)
{
    (void)t;
    s_ble_off_timer = NULL;
    port_hid_stop();
}

/* ── 무선 상태 ────────────────────────────────────────────────
 * WiFi와 BLE가 안테나를 공유한다. 지금은 어느 쪽도 켜지 않으므로
 * 로그만 남기고, BLE HID를 붙일 때 여기에 실제 on/off를 채운다. */
static void radio_apply(radio_need_t need)
{
    static radio_need_t cur = RADIO_OFF;
    if (cur == need) return;
    cur = need;
    port_radio_set((int)need);

    /* BLE 를 앱 나가자마자 내리면 폰 목록에서 즉시 사라지고 연결도 끊긴다.
     * 앱을 잠깐 들락거릴 때마다 그러면 못 쓴다. 45초 유예를 두고,
     * 그 안에 다시 BLE 앱에 들어가면 없던 일이 된다. */
    if (need == RADIO_BLE) {
        if (s_ble_off_timer) { lv_timer_delete(s_ble_off_timer); s_ble_off_timer = NULL; }
        port_hid_start();
    } else if (!s_ble_off_timer) {
        s_ble_off_timer = lv_timer_create(ble_off_cb, 45000, NULL);
        lv_timer_set_repeat_count(s_ble_off_timer, 1);
    }
}


/* 배터리 일지. 화면을 꺼두고 밤새 놔둬도 배지가 스스로 남기게 한다 —
 * 케이블이 빠져 있으면 시리얼 로그를 받아 적을 데가 없다. */
static void batt_log_cb(lv_timer_t *t)
{
    (void)t;
    port_battery_log(s_veil ? "idle-off" : "idle-on");
    port_imu_idle_check();

    /* 🚨 꽂혀 있으면 재우지 않는다.
     * USB-Serial-JTAG 은 CPU 가 자면 같이 멈춘다 — 라이트슬립이 드는 순간
     * 로그가 통째로 끊겨서 기기가 멀쩡한지 죽었는지 구분이 안 된다
     * (0907 밤에 이것 때문에 검증 로그를 두 번 날렸다).
     * 꽂혀 있으면 어차피 배터리를 아낄 이유가 없다. */
    static bool held;
    bool plugged = port_battery_plugged();
    if (plugged != held) { port_pm_hold(plugged); held = plugged; }
    port_uptime_mark();
}

static void housekeep_cb(lv_timer_t *t)
{
    (void)t;
    if (port_rec_active()) return;      /* 녹음 중엔 건드리지 않는다 */
    /* 🚨 시각도 여기서 같이 챙긴다. 예전엔 부팅 때 한 번, 그것도 "시각이
     * 아예 없을 때만" 맞췄다 — 그래서 한 번 맞추면 영영 다시 안 맞췄고
     * RTC 칩이 없는 이 보드는 하루에 몇 분씩 앞섰다(0910: 폰보다 3~4분).
     * 부르는 건 공짜다 — 안쪽에서 한 시간이 안 지났으면 그냥 돌아온다. */
    port_time_autosync();
}

/* ── 쪽 넘기기 ────────────────────────────────────────────────
 * 🚨 LVGL 제스처를 쓰려다 실패했다. 제스처는 눌린 객체에게만 가고, 올려
 * 보내려면 EVENT_BUBBLE 이 아니라 GESTURE_BUBBLE 이어야 하고, 화면이
 * 스크롤 가능하면 쓸기를 스크롤로 잡아 아예 안 보낸다 — 조건이 셋이나 되고
 * 하나만 어긋나도 조용히 아무 일도 안 일어난다(0909 에 세 번 헛짚었다).
 * 눌린 자리와 뗀 자리를 직접 재면 그런 게 없다. 손가락이 옆으로 크게
 * 움직였으면 쪽을 넘기고, 그 눌림은 앱을 열지 않는다. */
#define SWIPE_PX 60
static void build_home(void);
void launcher_show_home(void);
static int32_t s_press_x;
static bool    s_swiped;

static void home_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    s_press_x = p.x;
    s_swiped = false;
}

/* 넘겼으면 true — 부른 쪽은 앱을 열지 말아야 한다 */
static bool home_release_check(void)
{
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    int32_t dx = p.x - s_press_x;
    if (dx > -SWIPE_PX && dx < SWIPE_PX) return false;
    uint8_t was = s_page;
    if (dx < 0) s_page = (uint8_t)((s_page + 1) % PAGE_N);
    else        s_page = (uint8_t)((s_page + PAGE_N - 1) % PAGE_N);
    s_swiped = true;
    if (was != s_page) {
        /* 🚨 launcher_show_home() 은 이미 만들어둔 화면을 다시 띄우기만 한다.
         * 쪽이 바뀌면 타일이 달라지니 홈을 새로 지어야 한다 — 안 그러면
         * 안쪽 값만 바뀌고 화면은 그대로다(0909 에 그렇게 한참 헤맸다). */
        lv_obj_t *old = s_home;
        build_home();
        lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, 160, 0, false);
        if (old) lv_obj_delete_delayed(old, 400);   /* 넘어간 뒤에 치운다 */
    }
    return true;
}

static void home_swipe_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { home_press_cb(e); return; }
    home_release_check();
}

static void app_btn_cb(lv_event_t *e)
{
    /* 옆으로 쓸어 쪽을 넘긴 것이면 앱을 열지 않는다 */
    if (home_release_check()) return;
    launcher_open((const badge_app_t *)lv_event_get_user_data(e));
}

/* 앱이 "지금은 끄지 마라"고 잠깐 붙잡을 수 있다.
 * 타이머가 카운트다운 중일 때처럼, 안 만져도 화면이 살아 있어야 하는 경우. */
/* ── 자동 회전 ────────────────────────────────────────────────
 * 화면을 통째로 돌리면 앱마다 좌표를 뒤틀 필요가 없다.
 *
 * 어지럽지 않게 하는 규칙 세 개:
 *   · 90도 단위로만 딱딱 끊어 돈다 (비스듬한 각도로는 안 따라간다)
 *   · 경계에서 60도 넘게 확실히 넘어가야 바뀐다 (손목 흔들림 무시)
 *   · 그 자세를 0.8초 이상 유지해야 한다. 손가락이 닿아 있으면 안 돈다
 * 센서 읽기는 5Hz 라 전력은 사실상 공짜다. */
/* 기본은 꺼둔다. 실기 로그를 보니 눕혀 놨는데도 평면 성분이 500mg 나와서
 * (센서 축이 화면과 어떻게 맞물렸는지 아직 모른다) 멋대로 돌아갔다.
 * 게다가 화면만 돌고 터치 좌표가 따라 도는지 확인이 안 됐다 —
 * 어긋나면 아무것도 못 누른다. 설정에서 켜서 확인한 뒤에 기본으로 돌린다. */
static int   s_rot;             /* 0~3 */
static int   s_rot_cand;
static int   s_rot_hold;

void launcher_set_autorotate(bool on)
{
    s_autorotate = on;
    /* 꺼놨으면 200ms 타이머가 돌 이유가 없다. 켤 때 되살린다. */
    if (s_rotate_timer) {
        if (on && !s_veil) lv_timer_resume(s_rotate_timer);
        else               lv_timer_pause(s_rotate_timer);
    }
}
bool launcher_get_autorotate(void)    { return s_autorotate; }

static void rotate_poll(lv_timer_t *t)
{
    (void)t;
    if (!s_autorotate || s_veil) return;

    lv_indev_t *in = lv_indev_get_next(NULL);
    if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) return;  /* 만지는 중엔 금지 */

    float deg;
    if (!port_imu_angle(&deg)) return;
    /* 🚨 이 코드는 아직 검증 안 됐다. 0905 에 기준 방향을 MADCTL 로 180도
     * 뒤집었는데 IMU 는 기판에 고정이라 안 따라온다 — 자동회전을 다시 켤 땐
     * 여기 각도에 180 을 더해야 맞을 것이다(구슬게임에서 같은 일을 겪었다).
     * 터치가 같이 도는지도 아직 확인 전. */
    /* 확실히 세워 들었을 때만 본다. 눕힌 상태에서 도는 게 제일 나쁘다. */
    if (!port_imu_upright()) return;

    /* 중력 방향을 네 구역으로 나눈다. 경계는 45도지만 15도를 더 요구해
     * 경계에서 오락가락하지 않게 한다. */
    float a = deg;
    while (a < 0) a += 360.f;
    int want = s_rot;
    for (int k = 0; k < 4; k++) {
        float centre = k * 90.f;
        float d = a - centre;
        while (d > 180.f)  d -= 360.f;
        while (d < -180.f) d += 360.f;
        if (fabsf(d) < 30.f) { want = k; break; }     /* 중심에서 30도 안이면 그 자세 */
    }

    if (want != s_rot_cand) { s_rot_cand = want; s_rot_hold = 0; return; }
    if (want == s_rot) return;
    if (++s_rot_hold < 4) return;                     /* 5Hz x 4 = 0.8초 유지 */

    s_rot = want;
    s_rot_hold = 0;
    lv_display_set_rotation(lv_display_get_default(),
        (lv_display_rotation_t)(LV_DISPLAY_ROTATION_0 + s_rot));
    port_log(TAG, "화면 회전 %d도", s_rot * 90);
}

static void heap_cb(lv_timer_t *t)
{
    (void)t;
    port_heap_report(s_current ? s_current->name : "idle");
}

static void batt_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_batt) return;
    int p = port_battery_percent();
    if (p < 0) {
        lv_label_set_text(s_batt, "");           /* 배터리 없으면 아예 안 보인다 */
        return;
    }
    bool plug = port_battery_plugged();

    /* 🔋 값이 그대로면 아무것도 안 한다. 예전엔 20초마다 무조건 라벨을
     * 다시 써서, 안 바뀐 숫자 때문에 그 자리를 계속 다시 그렸다.
     * 이걸 막아두니 더 자주 봐도 예전보다 덜 그린다.
     * 🚨 다만 홈을 새로 지으면 라벨도 새것이라 한 번은 반드시 써야 한다.
     * 안 그러면 LVGL 이 라벨에 넣어두는 기본 글자("Text")가 그대로 남는다
     * (0909 에 쪽 넘기기를 넣고 그렇게 됐다). build_home 이 이걸 지운다. */
    if (p == s_prev_p && plug == s_prev_plug) return;
    s_prev_p = p;
    s_prev_plug = plug;

    lv_label_set_text_fmt(s_batt, plug ? "%d%% +" : "%d%%", p);

    /* 10단으로 나눈 색. 빨강에서 초록까지 한 칸씩 넘어간다.
     * 숫자를 읽기 전에 색만 봐도 대충 안다. */
    static const uint32_t LEVEL[11] = {
        0xE83B3B,  /*   0~9  */
        0xE85C3B,  /*  10~19 */
        0xE8783B,  /*  20~29 */
        0xE8963B,  /*  30~39 */
        0xE8B43B,  /*  40~49 */
        0xE0CC3B,  /*  50~59 */
        0xC9D63A,  /*  60~69 */
        0xA8D44A,  /*  70~79 */
        0x86D25C,  /*  80~89 */
        0x66D073,  /*  90~99 */
        0x4FCF8A,  /*  100   */
    };
    int idx = p / 10;
    if (idx > 10) idx = 10;
    uint32_t col = plug ? 0x4FC3F7 : LEVEL[idx];   /* 꽂혀 있으면 하늘색 */
    lv_obj_set_style_text_color(s_batt, lv_color_hex(col), 0);
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    if (home_release_check()) return;
    launcher_open(&app_settings);
}

static void build_home(void)
{
    s_batt = NULL;
    s_home = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_style_bg_color(s_home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);

    /* The wallpaper is drawn, not stored. A full-screen 466x466 picture costs
     * 424 KB of flash and this board has 890 KB of headroom left; a gradient
     * costs nothing and reads the same behind a ring of icons. */
    lv_obj_t *wall = lv_obj_create(s_home);
    lv_obj_remove_style_all(wall);
    lv_obj_remove_flag(wall, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(wall, 466, 466);
    lv_obj_center(wall);
    lv_obj_set_style_radius(wall, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(wall, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(wall, lv_color_hex(0x101822), 0);
    lv_obj_set_style_bg_grad_color(wall, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(wall, LV_GRAD_DIR_VER, 0);

    /* 앱은 120도 간격. 아이콘은 앱 화면을 그대로 줄인 그림이라
     * 글리프보다 뭐가 뭔지 바로 보인다. */
    for (size_t i = 0; i < APP_CNT; i++) {
        const badge_app_t *a = s_apps[i];
        float ang = (float)(-M_PI / 2.0 + i * (2.0 * M_PI / APP_CNT));
        int x = (int)(cosf(ang) * RING_R);
        int y = (int)(sinf(ang) * RING_R);

        lv_obj_t *tile = lv_image_create(s_home);
        lv_image_set_src(tile, a->art);
        lv_obj_set_size(tile, ICON_D, ICON_D);
        lv_image_set_scale(tile, ICON_D * 256 / 120);   /* 원본 120px 을 맞춰 줄인다 */
        lv_image_set_pivot(tile, 60, 60);
        lv_obj_align(tile, LV_ALIGN_CENTER, x, y);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        /* 🚨 제스처는 '눌린 객체' 에게만 간다. 아이콘 위에서 쓸면 아이콘이
         * 먹고 화면까지 안 올라와서 쪽이 안 넘어간다(0909).
         * LVGL 은 제스처에만 쓰는 별도 플래그를 본다 — EVENT_BUBBLE 이
         * 아니라 GESTURE_BUBBLE 이다. 그걸로 눌림은 아이콘이 그대로 먹고
         * 쓸기만 화면으로 올라간다. */
        lv_obj_add_event_cb(tile, home_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(tile, app_btn_cb, LV_EVENT_RELEASED, (void *)a);

        /* LVGL 기본 폰트엔 굵은 판이 없다. 1px 어긋나게 두 번 찍어 굵게 만들고,
         * 배경이 사진이라 뒤에 어두운 그림자를 한 장 깔아 글씨를 띄운다. */
        int ny = y + ICON_D / 2 + 21;
        for (int k = 0; k < 3; k++) {
            static const int8_t OX[3] = { 1, 0, 1 };
            static const int8_t OY[3] = { 2, 0, 0 };
            lv_obj_t *nm = lv_label_create(s_home);
            lv_label_set_text(nm, a->name);
            lv_obj_set_style_text_font(nm, APP_CNT <= 5 ? &lv_font_montserrat_20
                                                        : &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(nm, k == 0 ? lv_color_black() : lv_color_hex(0xF2F5F8), 0);
            lv_obj_set_style_text_opa(nm, k == 0 ? 150 : LV_OPA_COVER, 0);
            lv_obj_align(nm, LV_ALIGN_CENTER, x + OX[k], ny + OY[k]);
        }
    }

    /* 쪽 표시 — 몇 쪽이 있고 지금 어디인지. 점 두 개면 충분하다. */
    for (int k = 0; k < PAGE_N; k++) {
        lv_obj_t *d = lv_obj_create(s_home);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(k == s_page ? 0xE8ECF0 : 0x4A4A52), 0);
        lv_obj_align(d, LV_ALIGN_CENTER, (k - (PAGE_N - 1) * 0.5f) * 18, 196);
    }

    /* 좌우로 쓸면 쪽이 넘어간다.
     * 🚨 화면은 기본이 '스크롤 가능' 이라, LVGL 이 쓸기를 스크롤로 잡아
     * 제스처를 아예 안 보낸다(indev_gesture 첫 줄에서 되돌아간다).
     * 스크롤을 끄면 그제야 제스처가 온다. */
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home, home_swipe_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_home, home_swipe_cb, LV_EVENT_RELEASED, NULL);

    /* 설정은 한가운데 톱니. 앱 타일을 하나 더 늘리면 셋의 균형이 깨진다. */
    lv_obj_t *gear = lv_image_create(s_home);
    lv_image_set_src(gear, &icon_gear);
    lv_obj_set_style_image_recolor(gear, lv_color_hex(0xD6DEE6), 0);
    lv_obj_set_style_image_recolor_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(gear, 22);
    lv_obj_center(gear);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear, home_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(gear, settings_cb, LV_EVENT_RELEASED, NULL);

    /* 배터리 — 그림은 자리만 먹는다. 숫자 하나면 충분하고,
     * 톱니 바로 밑이라 눈에 걸리지도 않는다. */
    s_batt = lv_label_create(s_home);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_20, 0);
    lv_obj_align(s_batt, LV_ALIGN_CENTER, 0, 44);
    s_prev_p = -2;              /* 새 라벨이니 한 번은 반드시 쓴다 */
    batt_refresh(NULL);
    /* 화면이 꺼지면 이 타이머는 멈춘다(idle_timers). 그러니 켜져 있는 동안만
     * 도는 셈이고, 그때 CPU 는 어차피 화면 때문에 깨어 있다. PMU 를 한 번
     * 읽는 건 I2C 두 바이트라 사실상 공짜다. */
    /* 🚨 홈을 새로 지을 때마다 타이머를 또 만들면 쪽을 넘길 때마다 하나씩
     * 쌓인다 — 지워지지 않고 영원히 돈다. 먼저 치운다. */
    if (s_batt_timer) lv_timer_delete(s_batt_timer);
    s_batt_timer = lv_timer_create(batt_refresh, 5000, NULL);
}

/* ── 열고 닫는 애니메이션 ────────────────────────────────────
 * 아이콘에서 화면이 자라나고, 나갈 때 도로 오므라든다.
 * LVGL 의 화면 전환 효과는 밀어내기밖에 없어서, 앱 내용을 담은 판을
 * 따로 두고 그 판의 배율과 투명도를 직접 움직인다. */
#define ANIM_MS_OPEN   190
#define ANIM_MS_CLOSE  150

static lv_obj_t *s_stage;
static bool      s_closing;

/* ── 홈 손잡이 ────────────────────────────────────────────────
 * 아이폰의 그 막대. 잡고 위로 밀면 홈으로 나간다.
 * 앱 화면 위에 따로 올려두기 때문에, 마우스 트랙패드처럼 화면 전체를
 * 쓰는 앱에서도 **손가락이 손잡이에서 출발했을 때만** 먹는다.
 * 가운데를 문지르다가 아래로 지나가는 건 손잡이가 못 본다. */
#define HANDLE_W    104
#define HANDLE_H    6
#define HANDLE_Y    (-10)   /* 거의 바닥에 붙인다 — 다마고치 버튼을 가리면 안 된다 */
#define SWIPE_UP    45      /* 이만큼 올리면 나간다 */
/* 🚨 잡는 자리는 한 군데서만 정한다. 앱이 손잡이 제스처를 직접 받을 때
 * (launcher_handle_zone) 여기와 어긋나면 보이는 곳과 먹는 곳이 달라진다. */
#define HANDLE_W    240
#define HANDLE_H    64

static void (*s_handle_action)(void);
static lv_obj_t *s_handle;      /* 보이는 곡선 */
static lv_obj_t *s_handle_hit;  /* 실제로 손가락을 받는 투명한 판 */
static int32_t   s_handle_y0;
static bool      s_handle_armed;

/* 🚨 예전엔 여기서 static s_handle 을 썼다. 그런데 손잡이는 화면마다
 * 따로 만들어지고(홈에 하나, 잠금화면에 하나, 앱 화면에 하나), 포인터는
 * 하나뿐이었다. 앱을 닫으면 close_done_cb 가 그 포인터를 NULL 로 만드는데
 * 홈 화면의 손잡이는 멀쩡히 살아서 콜백을 물고 있다 → 그걸 만지는 순간
 * NULL 에 스타일을 걸어 죽었다(0906 코어덤프로 잡음: obj=0x0,
 * lv_obj_set_local_style_prop, 하던일=화면켬).
 *
 * 이제 각 손잡이가 자기 호를 user_data 로 들고 다닌다. 남의 포인터를
 * 쳐다보지 않으니 어느 화면이 지워지든 상관없다. */
static void handle_cb(lv_event_t *e)
{
    lv_obj_t *arc = (lv_obj_t *)lv_event_get_user_data(e);
    if (!arc) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_handle_y0 = p.y;
        s_handle_armed = false;
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dy = p.y - s_handle_y0;
        if (dy > 0) dy = 0;
        if (dy < -60) dy = -60;
        lv_obj_set_style_translate_y(arc, dy, 0);   /* 손가락을 따라온다 */
        if (dy <= -SWIPE_UP) s_handle_armed = true;
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_set_style_translate_y(arc, 0, 0);
        lv_obj_set_style_arc_opa(arc, 150, LV_PART_MAIN);
        /* 동작도 손잡이마다 다르다 — 잠금화면은 launcher_show_home,
         * 앱 화면은 launcher_home. static 하나로 두면 마지막에 붙인 쪽이
         * 이긴다. 게다가 lock_screen() 은 화면을 캐시해서 두 번째부터
         * 손잡이를 다시 안 붙이므로, 앱을 한 번 열었다 나오면 잠금해제가
         * launcher_home 을 부르고 그건 앱이 없으면 그냥 되돌아나온다
         * = 잠금이 안 풀린다(0906). 그래서 자기 몸에 붙여둔 걸 쓴다. */
        void (*act)(void) = (void (*)(void))lv_obj_get_user_data(arc);
        if (!act) act = s_handle_action;          /* 옛 경로 대비 */
        if (s_handle_armed && act) act();
    }
}

/* 게임처럼 화면 아래까지 쓰는 앱에서는 손잡이가 조작을 뺏는다.
 * 그때는 숨기고, 나가는 건 PWR(홈) 이나 앱 안의 뒤로 단추로 한다. */
void launcher_handle_show(bool on)
{
    if (!s_handle || !s_handle_hit) return;
    /* 화면이 지워졌으면 이 포인터는 죽은 자리다. LVGL 에 물어보고 쓴다. */
    if (!lv_obj_is_valid(s_handle) || !lv_obj_is_valid(s_handle_hit)) {
        s_handle = NULL; s_handle_hit = NULL;
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_handle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_handle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 그림은 두고 잡는 판만 걷는다.
 * 🚨 트랙패드처럼 화면 전체가 입력인 앱은 손잡이 판이 그 자리를 먹으면 안
 * 된다 — 아래쪽에서 시작한 문지르기가 통째로 죽는다. 그런 앱은 이걸 켜고
 * 아래 세 함수로 제 손으로 판정한다. */
void launcher_handle_passthrough(bool on)
{
    if (!s_handle_hit || !lv_obj_is_valid(s_handle_hit)) return;
    if (on) lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_remove_flag(s_handle_hit, LV_OBJ_FLAG_HIDDEN);
}

bool launcher_handle_zone(int32_t x, int32_t y)
{
    return y >= 466 - HANDLE_H
        && x >= (466 - HANDLE_W) / 2
        && x <  (466 + HANDLE_W) / 2;
}

/* 손가락을 따라 그림을 끌고, 홈으로 갈 만큼 올렸는지 알려준다. */
bool launcher_handle_drag(int32_t dy)
{
    if (!s_handle || !lv_obj_is_valid(s_handle)) return false;
    if (dy > 0) dy = 0;
    if (dy < -60) dy = -60;
    lv_obj_set_style_translate_y(s_handle, dy, 0);
    lv_obj_set_style_arc_opa(s_handle, LV_OPA_COVER, LV_PART_MAIN);
    return dy <= -SWIPE_UP;
}

void launcher_handle_drop(void)
{
    if (!s_handle || !lv_obj_is_valid(s_handle)) return;
    lv_obj_set_style_translate_y(s_handle, 0, 0);
    lv_obj_set_style_arc_opa(s_handle, 150, LV_PART_MAIN);
}

/* 🚨 자리를 여기서 한 번만 정한다. dy -186 에서 원의 반폭이 140px 이라
 * dx -108 이면 바깥끝이 130px — 안 잘린다. 더 위로 올리면 잘린다. */
static void back_btn_cb(lv_event_t *e)
{
    void (*act)(void) = (void (*)(void))lv_event_get_user_data(e);
    if (act) act();
}

lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void))
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 60, 40);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, -108, -186);
    lv_obj_add_event_cb(b, back_btn_cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD2D8E4), 0);
    lv_obj_center(l);
    return b;
}

void launcher_handle_add(lv_obj_t *parent, void (*action)(void))
{
    s_handle_action = action;   /* 남겨두지만 콜백은 이제 안 본다 */
    /* 직선 막대는 둥근 몸에 안 어울린다. 테두리를 따라 도는 짧은 호로 그린다.
     * LVGL 각도는 3시가 0도, 시계방향이라 90도가 바닥이다. */
    s_handle = lv_arc_create(parent);
    lv_obj_set_size(s_handle, 438, 438);
    lv_obj_center(s_handle);
    lv_arc_set_bg_angles(s_handle, 82, 98);
    lv_arc_set_value(s_handle, 0);
    lv_obj_remove_style(s_handle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_handle, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_handle, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_handle, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_handle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_handle, 150, LV_PART_MAIN);
    /* 이 손잡이가 눌렸을 때 할 일을 자기 몸에 붙여둔다 */
    lv_obj_set_user_data(s_handle, (void *)action);

    /* 호는 얇아서 못 잡는다. 아래쪽에 투명한 판을 따로 깔아 그걸로 받는다. */
    s_handle_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(s_handle_hit);
    lv_obj_set_size(s_handle_hit, HANDLE_W, HANDLE_H);
    lv_obj_align(s_handle_hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_handle_hit, LV_OBJ_FLAG_CLICKABLE);
    /* 자기 호를 들려 보낸다 — static 을 쳐다보면 다른 화면 것이 섞인다 */
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_PRESSED,  s_handle);
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_PRESSING, s_handle);
    lv_obj_add_event_cb(s_handle_hit, handle_cb, LV_EVENT_RELEASED, s_handle);
}

/* 옛 화면은 LVGL 이 애니메이션을 끝내고 스스로 지운다(auto_del).
 * 우리가 중간에 끼어들어 지우면, 아직 그 화면을 참조하던 애니메이션이
 * 지워진 메모리를 밟는다 — 시뮬에서 그대로 죽었다. */
static void close_done_cb(void *a)
{
    (void)a;
    s_closing = false;
    port_heap_report("home");
    lock_stop();
    s_app_scr = NULL;
    s_stage = NULL;
    s_handle = NULL;
    s_handle_hit = NULL;
    s_current = NULL;
    radio_apply(RADIO_OFF);
}

void launcher_open(const badge_app_t *app)
{
    if (s_closing) return;   /* 닫는 중엔 무시한다. 곧 홈이 뜬다 */
    if (s_app_scr) return;              /* 이미 앱 안 */
    port_log(TAG, "open %s", app->name);

    radio_apply(app->radio);

    s_app_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_app_scr);
    lv_obj_set_style_bg_color(s_app_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_app_scr, LV_OPA_COVER, 0);

    /* 앱 내용은 이 판 위에 올린다 */
    s_stage = lv_obj_create(s_app_scr);
    lv_obj_remove_style_all(s_stage);
    lv_obj_set_size(s_stage, 466, 466);
    lv_obj_center(s_stage);
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(s_stage, 233, 0);
    lv_obj_set_style_transform_pivot_y(s_stage, 233, 0);

    s_current = app;
    app->enter(s_stage);
    port_heap_report(app->name);
    launcher_handle_add(s_app_scr, launcher_home);   /* 앱 내용 위에 올린다 */

    /* 배율 애니메이션은 화면 전체를 매 프레임 확대해 다시 그린다 —
     * ESP32 에선 눈에 띄게 굼떴다. LVGL 이 최적화해둔 화면 페이드로 간다. */
    lv_screen_load_anim(s_app_scr, LV_SCR_LOAD_ANIM_FADE_IN, ANIM_MS_OPEN, 0, false);
}

void launcher_home(void)
{
    /* 화면이 꺼져 있으면 홈으로 가는 대신 깨운다.
     * 안 보이는 화면에서 앱이 닫히면 사용자는 뭐가 일어났는지 모른다. */
    if (s_veil) { screen_wake(); return; }
    if (!s_app_scr || !s_stage || s_closing) return;
    port_crumb(CRUMB_HOME);
    port_log(TAG, "home");

    /* 앱 정리는 먼저 한다 — 타이머가 도는 채로 오므라들면 안 된다 */
    if (s_current && s_current->leave) s_current->leave();
    /* 🚨 앱이 잡아둔 깨움은 여기서 반드시 놓는다. 마우스 앱이 매 송신마다
     * 잡기만 하고 나갈 때 안 놓아서, 마우스에서 나온 뒤 홈이 몇 분씩 켜져
     * 있었다(0910 지적). 앱마다 제 손으로 놓게 두면 하나만 빠뜨려도 이렇게
     * 된다 — 문 닫는 자리에서 한 번에 거둔다.
     * 🚨 AWAKE_STOP·AWAKE_RING 은 안 건드린다. 그건 앱 밖에 사는 것들이라
     * (스톱워치가 돌거나 알람이 우는 중) 여기서 놓으면 안 된다. */
    launcher_keep_awake_by(AWAKE_APP, false);
    s_closing = true;

    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_FADE_IN, ANIM_MS_CLOSE, 0, true);
    lv_timer_t *t = lv_timer_create((lv_timer_cb_t)close_done_cb, ANIM_MS_CLOSE + 30, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void launcher_start(void)
{
    build_home();
    /* 켜면 먼저 잠금화면(시계). 홈은 손잡이를 올려야 나온다. */
    lv_obj_t *lk = lock_screen();
    lock_start();
    lv_screen_load_anim(lk, LV_SCR_LOAD_ANIM_FADE_IN, 260, 0, true);
    port_home_button_start(launcher_home);
    s_idle = lv_timer_create(idle_cb, 1000, NULL);
    s_heap_timer   = lv_timer_create(heap_cb, 10000, NULL);
    s_rotate_timer = lv_timer_create(rotate_poll, 200, NULL);
    if (!s_autorotate) lv_timer_pause(s_rotate_timer);   /* 기본 꺼짐 */
    lv_timer_create(batt_log_cb, 60000, NULL);   /* 1분마다. 일지는 5분에 한 줄 */
    /* 부팅 30초 뒤 한 번, 그 뒤 30분마다. 집 WiFi 에 들어오면 그 안에 걸린다. */
    lv_timer_t *up = lv_timer_create(housekeep_cb, 30000, NULL);
    lv_timer_set_repeat_count(up, 1);
    lv_timer_create(housekeep_cb, 30 * 60 * 1000, NULL);

}
