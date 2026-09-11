/* PC 시뮬레이터용 port 구현. 실기 코드는 건드리지 않는다. */
#include "display.h"
#include "port.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void port_lock(void)   {}
void port_unlock(void) {}

void port_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* stdout 은 프레임 파이프 전용이다. 로그는 stderr 로 보낸다. */
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* NVS 대신 파일. 시뮬 재실행 사이에도 다마고치 상태가 남는다. */
static void kv_path(const char *key, char *out, size_t n)
{
    snprintf(out, n, "/tmp/badge_kv_%s.bin", key);
}

bool port_kv_read(const char *key, void *out, size_t len)
{
    char path[256];
    kv_path(key, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(out, 1, len, f);
    fclose(f);
    return got == len;
}

void port_kv_write(const char *key, const void *in, size_t len)
{
    char path[256];
    kv_path(key, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(in, 1, len, f);
    fclose(f);
}

void port_home_button_start(void (*on_press)(void)) { (void)on_press; }
void port_radio_set(int need) { fprintf(stderr, "[radio] need=%d\n", need); }

/* ── 에뮬레이터 지원 (가상 시계) ──────────────────────────────
 * 실제로 기다리지 않는다. 시간을 밀어버리면 에뮬이 최고 속도로 돈다. */
#include <stdlib.h>

uint32_t g_sim_us;

uint32_t port_micros(void) { return g_sim_us; }
void     port_delay_us(uint32_t us) { g_sim_us += us; }
void    *port_big_alloc(size_t n) { return malloc(n); }

void port_task_start(const char *name, void (*fn)(void *), void *arg, int stack)
{
    (void)name; (void)fn; (void)arg; (void)stack;   /* 시뮬은 호출측이 직접 돌린다 */
}

/* 시뮬은 소리를 못 낸다. 몇 Hz가 언제 울렸는지만 남긴다. */
static uint32_t g_tone_hz;
void port_tone_init(void) {}
void port_tone_freq(uint32_t hz) { g_tone_hz = hz; }
void port_tone_enable(bool on) { if (on) fprintf(stderr, "[tone] %u Hz\n", g_tone_hz); }
void port_tone_volume(int percent) { (void)percent; }

/* 시뮬은 코덱이 없으니 붙잡을 것도 없다 */
void port_tone_hold(bool on) { (void)on; }
void port_boot_btn_fake(uint32_t ms) { (void)ms; }
uint32_t port_boot_isr_count(void) { return 0; }
bool port_tone_codec_open(void) { return false; }
int  port_hid_forget_all(void) { return 0; }
/* 시뮬엔 IMU 가 없다. 손가락으로 만든 가짜 값만 쓴다. */
/* ── 가짜 IMU ────────────────────────────────────────────────
 * 🚨 시뮬에 IMU 가 없어서 기울기로 도는 것(구슬·브릭 기울기판·물 중력·
 * 에어마우스)이 통째로 시험 밖에 있었다. 실기에서만 터지는 버그가 이쪽에
 * 몰린 이유다(0909). 밖에서 값을 넣을 수 있게 두고, 안 넣으면 예전처럼
 * "IMU 없음" 으로 답한다 — 없는 기기도 그대로 흉내낼 수 있어야 하니까. */
static float g_imu_x, g_imu_y, g_imu_z = 1000.0f;
static bool  g_imu_on;

void sim_imu_set(float x, float y, float z)
{ g_imu_x = x; g_imu_y = y; g_imu_z = z; g_imu_on = true; }
void sim_imu_off(void) { g_imu_on = false; }

/* 자이로도 같은 방식으로 밖에서 넣는다. 안 넣으면 "자이로 없음" 이라
 * 에어마우스가 기울기로 되돌아간다 — 그 갈림길까지 시뮬에서 밟아본다. */
static float g_gyr_x, g_gyr_y, g_gyr_z;
static bool  g_gyr_have, g_gyr_on;

void sim_gyro_set(float x, float y, float z)
{ g_gyr_x = x; g_gyr_y = y; g_gyr_z = z; g_gyr_have = true; }
void sim_gyro_off(void) { g_gyr_have = false; }

void port_imu_gyro_enable(bool on) { g_gyr_on = on; }

bool port_imu_gyro(float *x, float *y, float *z)
{
    if (!g_gyr_on || !g_gyr_have) return false;
    if (x) *x = g_gyr_x;
    if (y) *y = g_gyr_y;
    if (z) *z = g_gyr_z;
    return true;
}

bool port_imu_accel3(float *x, float *y, float *z)
{
    if (!g_imu_on) { (void)x; (void)y; if (z) *z = 1000.0f; return false; }
    if (x) *x = g_imu_x;
    if (y) *y = g_imu_y;
    if (z) *z = g_imu_z;
    return true;
}

static int g_bright = 80;
void port_brightness_set(int percent) { g_bright = percent; }
int  port_brightness_get(void) { return g_bright; }

static net_state_t g_net = NET_IDLE;
void        port_time_sync_start(void) { g_net = NET_SYNCED; }   /* 시뮬은 PC 시계를 쓴다 */
net_state_t port_time_sync_state(void) { return g_net; }
const char *port_bt_status(void) { return "off"; }

/* ── BLE HID 시뮬 스텁 ────────────────────────────────────────
 * 실제로 보내는 데는 없지만 리포트를 찍어서 제스처 논리는 확인할 수 있다. */
static bool g_hid_on;
static int  g_hid_frames;

void port_hid_start(void) { g_hid_on = true;  g_hid_frames = 0; fprintf(stderr, "[hid] 광고 시작\n"); }
void port_hid_stop(void)  { g_hid_on = false; }
bool port_hid_connected(void) { return g_hid_on && ++g_hid_frames > 3; }  /* 잠깐 뒤 붙은 척 */
const char *port_hid_peer(void) { return port_hid_connected() ? "sim host" : "advertising"; }

/* 시뮬엔 상대가 없다. 감도는 기기별로 못 나누고 기본값 하나로 돈다. */
bool port_hid_peer_addr(uint8_t out[6]) { (void)out; return false; }

/* 시뮬엔 본딩이 없다. 화면을 눌러보려면 목록이 있어야 하므로 가짜로 준다. */
static char s_hostnm[3][17] = { "", "", "" };
int port_hid_hosts(hid_host_t *out, int max)
{
    static const uint8_t A[3][6] = {
        { 0xA4, 0x83, 0xE7, 0x11, 0x22, 0x33 },
        { 0x2C, 0xF0, 0xEE, 0x44, 0x55, 0x66 },
        { 0x9C, 0x8E, 0xCD, 0x77, 0x88, 0x99 },
    };
    int n = 3 > max ? max : 3;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].addr, A[i], 6);
        snprintf(out[i].name, sizeof out[i].name, "%s", s_hostnm[i]);
        out[i].here = (i == 0);
    }
    return n;
}
void port_hid_host_name_set(const uint8_t addr[6], const char *name)
{
    for (int i = 0; i < 3; i++) {
        hid_host_t t[3];
        port_hid_hosts(t, 3);
        if (memcmp(t[i].addr, addr, 6) == 0) {
            snprintf(s_hostnm[i], sizeof s_hostnm[i], "%s", name ? name : "");
            return;
        }
    }
}
void port_hid_host_pick(const uint8_t addr[6]) { (void)addr; }
void port_hid_host_any(void) { }

/* 시뮬엔 라디오가 없다. 화면을 눌러보려면 결과가 있어야 하므로 가짜로 준다.
 * 🚨 실기와 같은 시차(1초)를 둔다 — 곧바로 답하면 "훑는 중" 화면을 영영
 * 못 보고, 그 화면이 깨져 있어도 모른다. */
static uint32_t s_scan_t0;
static bool     s_scan_on;
void port_wifi_scan_start(void) { s_scan_on = true; s_scan_t0 = lv_tick_get(); }

int port_wifi_scan_result(wifi_found_t *out, int max)
{
    static const struct { const char *s; int8_t r; } FAKE[] = {
        { "sim-home",      -42 }, { "sim-hotspot",  -55 },
        { "neighbour-5G",  -71 }, { "cafe_guest",   -78 },
        { "printer-direct", -83 },
    };
    if (!s_scan_on || lv_tick_get() - s_scan_t0 < 1000) return -1;
    int n = (int)(sizeof FAKE / sizeof FAKE[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].ssid, sizeof out[i].ssid, "%s", FAKE[i].s);
        out[i].rssi = FAKE[i].r;
        out[i].saved = (i == 0) ? 1 : 0;
    }
    return n;
}

static char s_try[33], s_last_ok[33];
static uint32_t s_try_t0;
static bool s_try_on;
void port_wifi_try(const char *ssid, const char *pass)
{
    (void)pass;
    snprintf(s_try, sizeof s_try, "%s", ssid ? ssid : "");
    s_try_on = true; s_try_t0 = lv_tick_get();
}
int port_wifi_try_state(void)
{
    if (!s_try_on) return WIFI_TRY_FAIL;
    if (lv_tick_get() - s_try_t0 < 1500) return WIFI_TRY_BUSY;
    /* 시뮬에선 sim- 으로 시작하는 것만 붙는 척한다 — 실패 화면도 봐야 한다. */
    bool ok = strncmp(s_try, "sim-", 4) == 0;
    if (ok) snprintf(s_last_ok, sizeof s_last_ok, "%s", s_try);
    return ok ? WIFI_TRY_OK : WIFI_TRY_FAIL;
}
void port_wifi_last_ok(char *ssid, size_t ss) { snprintf(ssid, ss, "%s", s_last_ok); }

static char s_slot_ssid[WIFI_SLOTS][33];
void port_wifi_slot_set(int slot, const char *ssid, const char *pass)
{
    (void)pass;
    if (slot < 0 || slot >= WIFI_SLOTS) return;
    snprintf(s_slot_ssid[slot], sizeof s_slot_ssid[slot], "%s", ssid ? ssid : "");
}
void port_wifi_slot_clear(int slot)
{
    if (slot >= 0 && slot < WIFI_SLOTS) s_slot_ssid[slot][0] = '\0';
}
int port_wifi_slot_find(const char *ssid)
{
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (s_slot_ssid[i][0] && strcmp(s_slot_ssid[i], ssid) == 0) return i;
    return -1;
}
int port_wifi_slot_free(void)
{
    for (int i = 0; i < WIFI_SLOTS; i++) if (!s_slot_ssid[i][0]) return i;
    return WIFI_SLOTS - 1;
}
bool port_wifi_slot_get(int slot, char *ssid, size_t ss)
{
    if (slot < 0 || slot >= WIFI_SLOTS) { ssid[0] = '\0'; return false; }
    snprintf(ssid, ss, "%s", s_slot_ssid[slot]);
    return ssid[0] != '\0';
}

void port_hid_key(unsigned modifier, unsigned keycode)
{
    fprintf(stderr, "[hid] key mod=%u code=0x%02X\n", modifier, keycode);
}

void port_hid_type(const char *s)
{
    fprintf(stderr, "[hid] type \"%s\"\n", s ? s : "");
}

void port_hid_mouse(int dx, int dy, unsigned buttons, int wheel)
{
    if (dx || dy || buttons || wheel)
        fprintf(stderr, "[hid] dx=%d dy=%d btn=%u wheel=%d\n", dx, dy, buttons, wheel);
}

/* 시뮬은 브라우저가 세어준 손가락 수를 그대로 받는다 (N 명령) */
int g_touch_count = 1;
int port_touch_count(void) { return g_touch_count; }

/* 시뮬은 검은 덮개로만 표현한다 — 실기에선 화소가 진짜 꺼진다 */
void port_display_power(bool on) { (void)on; }

int port_pwr_key(void) { return 0; }   /* 시뮬은 W 명령으로 직접 토글한다 */

uint32_t port_hid_passkey(void) { return 0; }

void port_rtc_restore(void) {}   /* 시뮬은 PC 시계를 쓴다 */
void port_time_autosync(void) {}
static int g_tz = 9 * 60;
void port_set_tz_offset(int m) { g_tz = m; }
int  port_get_tz_offset(void) { return g_tz; }

bool port_imu_angle(float *deg) { (void)deg; return false; }   /* 시뮬엔 센서가 없다 */
bool port_imu_upright(void) { return false; }
bool port_imu_accel(float *x, float *y)
{
    float z;
    return port_imu_accel3(x, y, &z);
}

int  port_battery_percent(void) { return 76; }   /* 시뮬은 그럴듯한 값 */
bool port_battery_charging(void) { return false; }
bool port_battery_plugged(void) { return false; }
int  port_battery_minutes_left(void) { return 5 * 60 + 20; }

void port_power_off(void) { fprintf(stderr, "[axp] 전원 차단\n"); }

void port_heap_report(const char *when) { (void)when; }   /* 시뮬은 힙이 넉넉하다 */


/* ── 회의 버튼 (시뮬) ──────────────────────────────────────
 * 시뮬엔 폰이 없으니 가짜 폰을 하나 둔다. 명령을 받으면 잠시 뒤 회신을
 * 돌려줘서 화면 흐름을 실기와 같은 순서로 볼 수 있게 한다. */
static uint32_t s_meet_due;    /* 회신 예정 시각(us). 0 = 없음 */
static uint8_t  s_meet_next;

bool port_meet_link(void) { return true; }

bool port_meet_send(uint8_t cmd)
{
    port_log("meet", "명령 0x%02X (가짜 폰)", cmd);
    if (cmd == MEET_CMD_START_KO || cmd == MEET_CMD_START_EN) {
        s_meet_next = MEET_ACK_STARTED;
        s_meet_due  = port_micros() + 600000;      /* 0.6초 뒤 "마이크 열었다" */
    } else if (cmd == MEET_CMD_STOP) {
        s_meet_next = MEET_ACK_SAVED;
        s_meet_due  = port_micros() + 4000000;     /* 4초 뒤 "회의록 저장됨" */
    }
    return true;
}

bool port_meet_recv(uint8_t *status)
{
    if (!s_meet_due || port_micros() < s_meet_due) return false;
    s_meet_due = 0;
    if (status) *status = s_meet_next;
    return true;
}

size_t port_rec_capacity(void) { return 24u * 1024 * 1024; }

int  port_battery_mv(void) { return 3900; }
void port_battery_log(const char *what) { (void)what; }
void port_battery_mark(bool on) { (void)on; }


/* ── 녹음 (시뮬) ──────────────────────────────────────────
 * 시뮬엔 마이크도 플래시도 없다. 화면 흐름만 볼 수 있게 시늉만 낸다. */
static uint32_t s_rec_t0;
static bool     s_rec_on;
static int      s_rec_pend;

bool port_rec_start(int lang)
{
    (void)lang;
    port_log("rec", "녹음 시작 (시뮬)");
    s_rec_t0 = port_micros();
    s_rec_on = true;
    return true;
}
void     port_rec_stop(void)   { if (s_rec_on) { s_rec_on = false; s_rec_pend++; } }
bool     port_rec_active(void) { return s_rec_on; }
uint32_t port_rec_seconds(void){ return s_rec_on ? (port_micros() - s_rec_t0) / 1000000 : 0; }
uint32_t port_rec_free_seconds(void) { return 52 * 60; }
int      port_rec_pending(void)      { return s_rec_pend; }
void     port_rec_upload_try(void)   { s_rec_pend = 0; }
bool     port_rec_uploading(void)    { return false; }
const char *port_rec_upload_msg(void){ return ""; }

/* 시뮬엔 BLE 가 없다. 폰이 흔히 주는 값을 흉내낸다. */
int port_hid_interval_ms(void) { return 15; }

void port_big_free(void *p) { free(p); }

void port_pm_hold(bool on) { (void)on; }

void port_battery_journal_dump(void) {}

void port_reset_reason_note(int rr) { (void)rr; }

static int s_crumb_sim = -1;
void port_crumb(int what) { s_crumb_sim = what; }
int  port_crumb_now(void) { return s_crumb_sim; }
void port_uptime_mark(void) {}

/* 시뮬엔 패널이 없다. 설정 화면이 돌아가게만 흉내낸다. */
static int s_sim_xgap = 6;
void badge_display_set_xgap(int g) { s_sim_xgap = g; }
int  badge_display_get_xgap(void)  { return s_sim_xgap; }

/* 시뮬은 LVGL 입력장치를 자기가 만든다. 런처가 절전용으로 물어볼 때 쓴다. */
lv_indev_t *badge_display_indev(void) { return lv_indev_get_next(NULL); }

void port_cpu_mark(void) {}
void port_cpu_report(const char *w) { (void)w; }

void port_imu_idle_check(void) {}

void port_health_begin(void) {}
int  port_health_check(char *o, size_t n) { if (n) o[0] = 0; return 0; }
uint32_t port_health_errors(void) { return 0; }

/* ── USB 내보내기 흉내 ──────────────────────────────────────
 * 🚨 시뮬엔 USB 가 없다. 화면 흐름(꺼짐 → 기다림 → 붙음 → 뺐음)만 볼 수
 * 있게 가짜로 돌린다. 판을 짓는 쪽(usb_export.c)은 녹음 파티션을 읽으므로
 * 시뮬에 넣지 않는다 — 여기서 개수를 지어낸다. */
#include "usb_export.h"

static bool s_usb_on, s_usb_ej;
static int  s_usb_ticks;

void     usb_export_build(void) { s_usb_ticks = 0; }
bool     usb_export_read(uint32_t lba, uint8_t *out) { (void)lba; (void)out; return false; }
uint32_t usb_export_sectors(void) { return 49920; }
uint32_t usb_export_sector_size(void) { return 512; }
int      usb_export_files(void) { return 3; }

bool usb_msc_start(void) { s_usb_on = true; s_usb_ej = false; s_usb_ticks = 0; return true; }
void usb_msc_stop(void)  { s_usb_on = false; s_usb_ej = false; }
bool usb_msc_active(void) { return s_usb_on; }
bool usb_msc_mounted(void) { return s_usb_on && ++s_usb_ticks > 4; }
bool usb_msc_ejected(void) { return s_usb_ej; }
