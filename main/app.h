#pragma once
#include "lvgl.h"

/* 앱마다 필요한 무선 상태. WiFi와 BLE는 안테나를 공유하므로
 * 동시에 켜면 둘 다 느려진다. 런처가 앱 전환 시점에 정리한다. */
typedef enum {
    RADIO_OFF = 0,   /* 게임·오프라인 화면 */
    RADIO_BLE,       /* 에어마우스 (BLE HID) */
    RADIO_WIFI,      /* 시각 동기 등 잠깐 쓰는 경우 */
} radio_need_t;

typedef struct {
    const char  *name;
    const lv_image_dsc_t *art;  /* 런처 타일 그림. 없으면 icon 심볼을 쓴다 */
    const char  *icon;          /* LVGL 심볼 */
    lv_color_t (*tint)(void);   /* 런처 아이콘 색 */
    radio_need_t radio;
    bool         keep_awake;    /* 참이면 자동 꺼짐을 건너뛴다 */
    void (*enter)(lv_obj_t *root);  /* 앱 화면 구성 */
    void (*leave)(void);            /* 타이머·리소스 정리 */
} badge_app_t;

extern const badge_app_t app_mouse;
extern const badge_app_t app_clock;
extern const badge_app_t app_settings;
extern const badge_app_t app_keys;
extern const badge_app_t app_calc;
extern const badge_app_t app_games;
extern const badge_app_t app_meet;
extern const badge_app_t app_air;   /* 마우스와 같은 앱, 에어로 켜서 들어간다 */
extern const badge_app_t app_water; /* Games 안의 판을 홈에서 바로 연다 */
extern const badge_app_t app_orb;   /* 천체 — 안에서 다섯을 골라 본다 */

/* ── 멀티탭 키패드 ────────────────────────────────────────────
 * 옛날 핸드폰처럼 한 키를 여러 번 눌러 글자를 고른다. 맨 위 층에 떠서 부르는
 * 앱의 화면을 안 건드린다 — 끝나면 걷히고 앱은 그대로 있다.
 * done(text) 로 돌려주고, 손잡이를 올려 취소하면 done(NULL) 이다. */
void keypad_open(const char *title, const char *initial,
                 void (*done)(const char *text));
void keypad_close(void);
bool keypad_is_open(void);

/* WiFi 설정 — 배지에서 혼자 붙는다. 맨 위 층에 떠서 부르는 앱을 안 건드린다. */
void wifi_setup_open(void);

/* 녹음을 USB 드라이브로 내보내는 화면(apps/usb_screen.c).
 * 🚨 여기 들어가면 COM 포트가 사라진다 — usb_msc.c 의 설명을 보라. */
void usb_screen_open(void);
/* 붙은 적 있는 호스트 목록 — 골라서 옮겨 붙고, 길게 눌러 이름을 짓는다 */
void host_pick_open(void);

void launcher_start(void);
void launcher_screen_off(void);          /* 자동 꺼짐 — 터치로 깨어난다 */
void launcher_screen_off_manual(void);   /* 내가 껐다 — 버튼으로만 깨어난다 */
void launcher_screen_toggle(void);
void launcher_poweroff_notice(void);
void splash_show(void);
/* 잠금화면 (알티오라 느와르 시계). 화면을 켜면 여기로 온다. */
lv_obj_t *lock_screen(void);
void      lock_start(void);
void      lock_stop(void);
void      launcher_show_home(void);
/* 잠금 해제 — 열려 있던 앱이 있으면 그리로, 없으면 홈으로 */
void      launcher_unlock(void);
void      launcher_show_lock(void);
/* 되돌아갈 데가 있는 화면에 붙이는 뒤로가기.
 * 🚨 **앱의 첫 화면엔 붙이지 않는다** — 거기서 나가는 문은 손잡이고, 단추를
 * 또 두면 둘 중 뭐가 맞는지 헷갈린다. 한 겹 더 들어간 화면에만 붙인다.
 * 자리는 왼쪽 위 한 곳으로 고정한다 — 앱마다 다른 데 있으면 매번 찾아야 한다. */
lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void));

void      launcher_handle_add(lv_obj_t *parent, void (*action)(void));
void      launcher_handle_show(bool on);   /* 게임 중엔 숨긴다 */
/* 손잡이 그림은 두고 잡는 판만 걷는다 — 앱이 제 손으로 판정할 때 */
void      launcher_handle_passthrough(bool on);
bool      launcher_handle_zone(int32_t x, int32_t y);
bool      launcher_handle_drag(int32_t dy);   /* 끌고, 홈까지 올렸나 */
void      launcher_handle_drop(void);
void settings_load(void);
void launcher_keep_awake(bool on);
/* 여럿이 동시에 잡을 수 있다. 다 놓아야 화면이 꺼진다. */
#define AWAKE_APP   0    /* 앱 하나가 통째로(마우스 등) */
#define AWAKE_STOP  1    /* 스톱워치가 도는 중 */
#define AWAKE_RING  2    /* 알람·타이머가 우는 중 */
void launcher_keep_awake_by(int who, bool on);

/* ── 시계 앱의 쪽들 ─────────────────────────────────────────── */
void timer_build(lv_obj_t *root);
void timer_free(void);
void stopwatch_build(lv_obj_t *root);
void stopwatch_free(void);
void alarm_build(lv_obj_t *root);
void alarm_free(void);
/* 🚨 알람만은 앱 밖에서도 돈다 — 런처가 매초 부른다. */
void alarm_tick(void);

/* 🚨 타이머 콜백 안에서 lv_timer_set_period() 를 부르지 마라.
 * 안쪽에서 lv_timer_handler_resume() 을 불러서, 처리기가 그 자리에서
 * 무한히 다시 돈다 — CPU 를 100%% 물고 늘어진다(0909 에 절전하려다 그랬다).
 * 값이 같아도 마찬가지라 "바뀔 때만 세우기"로도 못 막는다.
 * 화면이 꺼졌을 때 덜 일하고 싶으면 주기를 그대로 두고 N번에 한 번만
 * 일해라 — 효과는 같고 안전하다. */
void launcher_screen_on(void);   /* 알람 등 — 꺼져 있으면 켠다 */
bool launcher_screen_is_off(void);   /* 꺼져 있으면 폴링을 늦춘다 */
void launcher_set_autorotate(bool on);
bool launcher_get_autorotate(void);
void launcher_set_timeout(int seconds);   /* 0 = 안 끔 */
int  launcher_get_timeout(void);
void launcher_home(void);
void launcher_open(const badge_app_t *app);
