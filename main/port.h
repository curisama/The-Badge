#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 실기(ESP32)와 PC 시뮬레이터가 같은 UI 코드를 쓰기 위한 얇은 층.
 * 여기 없는 것을 앱 코드가 직접 부르면 시뮬에서 안 돌아간다. */

void port_lock(void);       /* LVGL 뮤텍스 */
void port_unlock(void);
void port_log(const char *tag, const char *fmt, ...);

bool port_kv_read(const char *key, void *out, size_t len);
void port_kv_write(const char *key, const void *in, size_t len);

void port_home_button_start(void (*on_press)(void));

/* 무선 상태 전환. 시뮬에선 로그만 남는다. */
void port_radio_set(int need);

/* 에뮬레이터용. 마이크로초 단위 시계와 대기.
 * 시뮬에선 가상 시계라 기다리지 않고 시간만 밀어준다. */
uint32_t port_micros(void);
void     port_delay_us(uint32_t us);

/* 큰 버퍼(캔버스 등). 실기에선 PSRAM에서 잡는다. */
void *port_big_alloc(size_t n);
void  port_big_free(void *p);
void  port_heap_report(const char *when);   /* 힙 상태를 로그로 */

/* 배터리 잔량(%). 배터리가 없거나 못 읽으면 -1. */
void port_power_off(void);   /* AXP2101 에 소프트 종료를 명령한다 */
int port_battery_percent(void);
bool port_battery_charging(void);
bool port_battery_plugged(void);   /* USB 꽂혀 있나 (충전 끝났어도 참) */
/* 지금 속도로 갔을 때 남은 시간(분). 아직 못 재면 -1. */
int  port_battery_minutes_left(void);

/* 중력이 화면 평면에서 어느 쪽을 향하는지(도). 배지를 어떻게 들고 있든
 * "아래쪽"을 알 수 있다. 눕혀두면 평면 성분이 없어 false 를 돌려준다. */
bool port_imu_angle(float *deg);
bool port_imu_upright(void);
/* 안 쓰면 가속도계를 재운다. 런처가 주기적으로 부른다. */
void port_imu_idle_check(void);
/* 화면 평면에 실린 중력 (mg). 구슬을 굴리는 데 쓴다. */
bool port_imu_accel(float *x, float *y);   /* 화면이 세워져 있나 (눕혀 두면 false) */
bool port_imu_accel3(float *x, float *y, float *z); /* 세 축 그대로 (mg). 흔들림까지 본다 */

/* 각속도 (초당 도, dps). 축은 가속도와 같은 틀이다 — x 축 둘레 회전이 gx.
 *
 * 🚨 가속도로는 '자세' 밖에 모른다. 배지를 들고 팔을 옮겨도 자세가 그대로면
 * 아무 일도 안 일어난다. 자이로는 '돌아간 만큼' 을 주므로 진짜 에어마우스가
 * 된다.
 * 🔋 다만 자이로는 가속도계보다 열 배 넘게 먹는다(대략 1.5mA 대 0.03mA).
 * 그래서 기본으로 꺼져 있고, 쓸 사람이 켜고 나갈 때 끈다. 안 끄면 화면을
 * 꺼도 계속 돈다 — 가속도계에서 똑같이 당한 적이 있다. */
void port_imu_gyro_enable(bool on);
bool port_imu_gyro(float *x, float *y, float *z);

/* 지금 화면에 닿아 있는 손가락 수 (0~2). CST9217 이 2점까지 준다.
 * LVGL 은 한 점만 넘겨주므로 두 손가락 제스처는 이걸로 판단한다. */
int port_touch_count(void);

/* ── BLE HID 마우스 ─────────────────────────────────────────
 * 이 배지가 남에게 붙는 쪽이다. 표준 HID라 폰·PC·TV가 드라이버 없이 받는다. */
void        port_hid_start(void);      /* 스택 올리고 광고 시작 */
void        port_hid_stop(void);       /* 광고만 멈춘다. 연결은 유지 */
bool        port_hid_connected(void);
bool        port_hid_up(void);          /* 스택이 올라와 있나 (연결 여부와 별개) */
int         port_hid_forget_all(void); /* 짝지은 기기를 전부 잊는다 */

/* ── 붙은 적 있는 호스트 ──────────────────────────────────────
 * 🚨 BLE 본딩은 **주소만** 저장한다. 호스트 이름은 안 온다 — 배지가
 * 주변장치 쪽이라 상대 이름을 볼 일이 없다. 그래서 목록을 만들면
 * A4:83:E7:... 같은 게 세 줄 뜨고, 어느 게 집 PC 인지 알 수가 없다.
 * 키패드가 생겼으니 사람이 이름을 지어 붙인다(port_hid_host_name_set). */
#define HID_HOSTS_MAX 8
typedef struct {
    uint8_t addr[6];
    char    name[17];      /* 사람이 지은 이름. 비어 있으면 주소를 보인다 */
    bool    here;          /* 지금 이 기기에 붙어 있나 */
} hid_host_t;
int  port_hid_hosts(hid_host_t *out, int max);
void port_hid_host_name_set(const uint8_t addr[6], const char *name);
/* 🚨 그 호스트에게만 광고한다. 지금 링크는 끊는다.
 * 고른 것이 아닌 상대가 붙으면 그 자리에서 끊는다 — 화이트리스트만으로는
 * 스택에 따라 샌다. */
void port_hid_host_pick(const uint8_t addr[6]);
void port_hid_host_any(void);          /* 아무나 받는다(기본) */
const char *port_hid_peer(void);       /* 붙은 상대 표시용 */
void        port_hid_mouse(int dx, int dy, unsigned buttons, int wheel);
/* 지금 붙어 있는 상대의 주소(6바이트). 기기마다 다른 값을 기억하는 데 쓴다.
 * 🚨 ESP_GAP_BLE_AUTH_CMPL_EVT 는 재연결할 때도 뜨므로 붙을 때마다 갱신된다. */
bool        port_hid_peer_addr(uint8_t out[6]);

/* ── 주변 WiFi 훑기 (화면에서 쓴다) ───────────────────────────
 * 🚨 훑는 데 1~2초가 걸린다. 그동안 화면이 멎으면 안 되므로 **태스크에서
 * 돌리고 화면은 물어보기만 한다.** 콜백으로 알리면 LVGL 을 다른 실을 통해
 * 만지게 되는데 LVGL 은 그걸 못 견딘다.
 * 🚨 WiFi 를 올렸다 내리므로 BLE 와 겹치면 안 된다 — 부르는 앱의 radio 가
 * RADIO_OFF 여야 한다. */
#define WIFI_SCAN_MAX 16
typedef struct { char ssid[33]; int8_t rssi; uint8_t saved; } wifi_found_t;
void        port_wifi_scan_start(void);
/* -1 = 아직 도는 중, 0 이상 = 찾은 개수 */
int         port_wifi_scan_result(wifi_found_t *out, int max);

/* 칸(0~2)에 넣고 지운다. 화면에서 부른다. */
#define WIFI_SLOTS 3
void        port_wifi_slot_set(int slot, const char *ssid, const char *pass);
void        port_wifi_slot_clear(int slot);
/* 🚨 비밀번호는 절대 안 돌려준다 — 들어 있는지 여부만. */
bool        port_wifi_slot_get(int slot, char *ssid, size_t ss);
/* 이 SSID 가 들어 있는 칸. 없으면 -1 */
int         port_wifi_slot_find(const char *ssid);
/* 빌 칸. 다 찼으면 마지막 칸을 내준다(제일 오래된 것을 민다) */
int         port_wifi_slot_free(void);

/* ── 실제로 붙어본다 ──────────────────────────────────────────
 * 🚨 저장만 하고 붙어보질 않으면 비밀번호가 틀렸는지 알 길이 없다. 넣자마자
 * 한 번 붙어보고 결과를 보여준다.
 * 🚨 배지는 WiFi 에 **계속 붙어 있지 않는다** — 시각 맞추거나 올릴 때만 켰다
 * 끈다(전력). 그래서 '붙음' 은 지금 붙어 있다는 뜻이 아니라 **마지막으로 붙는
 * 데 성공한 망**이라는 뜻이다. 사용자가 알고 싶은 것("이거 되는 망인가")과
 * 맞는 뜻이다. */
void        port_wifi_try(const char *ssid, const char *pass);
#define WIFI_TRY_BUSY (-1)
#define WIFI_TRY_FAIL  0
#define WIFI_TRY_OK    1
int         port_wifi_try_state(void);
/* 마지막으로 붙는 데 성공한 망. 없으면 빈 문자열 */
void        port_wifi_last_ok(char *ssid, size_t ss);
/* 키 하나 눌렀다 떼기. modifier 비트: 1=Ctrl 2=Shift 4=Alt 8=GUI */
void        port_hid_key(unsigned modifier, unsigned keycode);
/* 아스키 문자열을 키보드로 쳐준다. 없는 글자는 건너뛴다. */
void        port_hid_type(const char *s);
uint32_t    port_hid_passkey(void);
/* 폰이 허락한 연결 간격(ms). 리포트를 이보다 자주 보내면 큐에 쌓인다. */
int         port_hid_interval_ms(void);   /* 짝짓기 확인 숫자. 없으면 0 */


/* ── 회의 버튼 ─────────────────────────────────────────────
 * 배지는 신호만 쏘고, 녹음은 폰의 루시드 앱이 한다. 그래서 배지 배터리가
 * 죽어도 회의록은 남는다. 명령을 보냈다고 화면을 바꾸면 안 된다 —
 * 앱이 "마이크 열었다"고 되쏴준 뒤에야 녹음 중으로 바꾼다. */
#define MEET_CMD_START_KO 0x01   /* 배지 → 앱 */
#define MEET_CMD_START_EN 0x11
#define MEET_CMD_STOP     0x02
#define MEET_ACK_STARTED  0x81   /* 앱 → 배지 */
#define MEET_ACK_SAVED    0xC0
#define MEET_ACK_FAIL     0xE1

bool port_meet_link(void);            /* 앱이 붙어서 구독 중인가 */
bool port_meet_send(uint8_t cmd);     /* 못 보내면 false */
bool port_meet_recv(uint8_t *status); /* 새 회신이 왔으면 담고 true */

/* ── 녹음 ───────────────────────────────────────────────────
 * 배지가 혼자 마이크를 열어 플래시에 쌓는다. 폰도 WiFi 도 필요 없다.
 * 올리는 건 나중 일이라, 회의 중에 끊겨서 날아갈 게 없다. */
size_t   port_rec_capacity(void);      /* 녹음 칸 크기(바이트). 0 = 파티션 없음 */
bool     port_rec_start(int lang);     /* 0=한국어 1=영어. 실패면 false */
void     port_rec_stop(void);
bool     port_rec_active(void);
uint32_t port_rec_seconds(void);       /* 지금 녹음 길이(초) */
uint32_t port_rec_free_seconds(void);  /* 더 담을 수 있는 초 */
int      port_rec_pending(void);       /* 아직 안 올린 녹음 수 */

/* 배터리 전압(mV). AXP2101 이 전류는 안 주고 전압만 준다. */
int  port_battery_mv(void);
/* 1분에 한 번 전압·잔량을 로그로 남긴다. 실제 소모 곡선을 쌓는 용도. */
void port_battery_log(const char *what);
/* 화면을 켜는 자리와 끄기 직전에 부른다. 그 둘 사이가 "화면을 켜 둔 구간" 이
 * 되어 소모를 잴 수 있다. 🚨 끄고 나서 부르면 화면X 로 적혀 짝이 깨진다. */
void port_battery_mark(bool screen_on);
/* 케이블 꽂았을 때 부팅 로그로 일지를 뱉는다. */
void port_battery_journal_dump(void);
/* CPU 가 일한 비율. mark 로 시작점을 찍고 report 로 그 구간을 본다.
 * 꽂아둔 채로도 절전 효과를 잴 수 있는 유일한 잣대다. */
/* 건강 검사 — "안 뻗었나"가 아니라 "사람이 보는 것들이 되나"를 본다.
 * begin 으로 세기 시작하고, check 가 0 이 아니면 뭔가 잘못된 것이다. */
void     port_health_begin(void);
int      port_health_check(char *out, size_t len);
uint32_t port_health_errors(void);

void port_cpu_mark(void);
void port_cpu_report(const char *when);
/* 재부팅 사유를 NVS 에 세어둔다. 브라운아웃인지 패닉인지 누계로 판단한다. */
void port_reset_reason_note(int rr);

/* 지금 뭘 하고 있는지 남긴다. 재부팅 뒤에 "죽기 직전에 뭐 하던 중"이 된다.
 * 전환 시점에만 부를 것 — 매 프레임 부르면 플래시가 닳는다. */
enum { CRUMB_BOOT, CRUMB_HOME, CRUMB_LOCK, CRUMB_SCR_OFF, CRUMB_SCR_ON,
       CRUMB_APP_OPEN, CRUMB_MOUSE, CRUMB_MEET, CRUMB_GAME, CRUMB_CALC,
       CRUMB_CLOCK, CRUMB_SETTINGS, CRUMB_REC };
void port_crumb(int what);
/* 지금 무엇으로 세고 있나. 잠갔다 풀 때 앱 구간을 이어 붙이는 데 쓴다. */
int  port_crumb_now(void);
void port_uptime_mark(void);   /* 60초마다. 다음 부팅 때 가동시간이 된다 */

/* 소리. 다마고치는 주파수 하나를 켜고 끄는 게 전부다. */
void port_tone_init(void);
void port_tone_freq(uint32_t hz);
void port_tone_enable(bool on);
void port_tone_volume(int percent);
/* 소리를 자주 낼 앱은 들어올 때 hold(true), 나갈 때 hold(false).
 * 코덱을 미리 열어둬 첫 소리를 놓치지 않는다. */
void port_tone_hold(bool on);

/* 시험용 — BOOT 버튼을 소프트웨어로 눌러본다. GPIO0 은 스트랩 핀이라
 * 짧게만, 화면이 켜진 동안에만 쓴다. */
void     port_boot_btn_fake(uint32_t ms);
uint32_t port_boot_isr_count(void);
bool     port_tone_codec_open(void);   /* 코덱을 실제로 잡고 있나 */

/* 시각 맞추기. 1.75인치 원형 화면에 비밀번호를 칠 수는 없으니
 * 네트워크 목록을 보여주는 대신 "붙어서 시계만 맞추고 끊는다". */
typedef enum {
    NET_NOCONF = 0,   /* 접속 정보 없음 */
    NET_IDLE,
    NET_CONNECTING,
    NET_SYNCED,
    NET_FAIL,
} net_state_t;

/* WiFi netif 는 한 번만 만들 수 있다. 시각 동기와 업로드가 나눠 쓴다. */
struct esp_netif_obj;
struct esp_netif_obj *badge_wifi_netif_once(void);
/* WiFi 는 한 번에 하나만. 시각 동기와 업로드가 겹치면 하드웨어를 두 번 올린다. */
/* 접속정보는 배지(NVS)에 있다. secrets.h 에 값이 있으면 구울 때 새로 넣고,
 * 비어 있으면 저장된 것을 그대로 쓴다 — 다른 컴퓨터에서 구워도 안 잃는다. */
void badge_creds_init(void);
bool badge_creds_wifi(char *ssid, size_t ss, char *pass, size_t ps);
/* WiFi 를 올린 뒤에 부른다 — 우리 칸이 비면 스택이 저장해 둔 것을 가져온다 */
bool badge_creds_wifi_live(char *ssid, size_t ss, char *pass, size_t ps);
/* 칸이 셋이다(집·핫스팟·하나 여유). 한 칸이라도 채워져 있나 */
bool badge_creds_wifi_any(void);
/* 🚨 WiFi 를 **올린 뒤에** 부른다. 한 번 훑어서 지금 잡히는 것 중 신호가
 * 센 쪽을 고른다 — 없는 망에 붙어보다 5~10초씩 날리지 않으려는 것이다. */
bool badge_wifi_pick(char *ssid, size_t ss, char *pass, size_t ps);

bool badge_wifi_take(uint32_t wait_ms);
void badge_wifi_give(void);

void        port_time_sync_start(void);
void        port_rtc_restore(void);    /* 저장해둔 시간대를 세운다 */
void        port_time_autosync(void);  /* 시각이 비었으면 부팅 때 한 번 맞춘다 */
void        port_set_tz_offset(int minutes);
int         port_get_tz_offset(void);
net_state_t port_time_sync_state(void);
const char *port_bt_status(void);

/* PWR 버튼. GPIO 가 아니라 AXP2101 의 PWRON 핀이라 I2C 로 읽는다.
 * 0 = 없음, 1 = 짧게, 2 = 길게(하드웨어가 전원을 끊기 직전) */
int port_pwr_key(void);

/* 화면 자체를 끄고 켠다. AMOLED 는 꺼진 화소가 전기를 안 먹어서
 * 밝기를 낮추는 것과 아예 끄는 것의 차이가 크다. */
void port_display_power(bool on);

/* 화면 밝기 0~100. AMOLED라 밝기가 곧 배터리다. */
void port_brightness_set(int percent);
int  port_brightness_get(void);

/* 라이트슬립을 잠깐 막는다. BLE 연결이나 I2S 녹음처럼 CPU 가 자면
 * 끊기는 일을 하는 동안만 잡는다. 중첩해서 불러도 된다. */
void port_pm_hold(bool on);

/* 백그라운드 루프. 시뮬에선 아무것도 안 하고 호출측이 직접 돌린다. */
void port_task_start(const char *name, void (*fn)(void *), void *arg, int stack);
