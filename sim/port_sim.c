/* The port implementation for the PC simulator. The hardware code is left alone. */
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
    /* stdout is the frame pipe and nothing else. Logs go to stderr. */
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* A file instead of NVS. The Tamagotchi state survives between simulator runs. */
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

/* ── emulator support (the virtual clock) ─────────────────────
 * It does not really wait. Pushing time forward runs the emulator flat out. */
#include <stdlib.h>

uint32_t g_sim_us;

uint32_t port_micros(void) { return g_sim_us; }
void     port_delay_us(uint32_t us) { g_sim_us += us; }
void    *port_big_alloc(size_t n) { return malloc(n); }

void port_task_start(const char *name, void (*fn)(void *), void *arg, int stack)
{
    (void)name; (void)fn; (void)arg; (void)stack;   /* in the simulator the caller drives it directly */
}

/* The simulator cannot make a sound. It only records what frequency sounded when. */
static uint32_t g_tone_hz;
void port_tone_init(void) {}
void port_tone_freq(uint32_t hz) { g_tone_hz = hz; }
void port_tone_enable(bool on) { if (on) fprintf(stderr, "[tone] %u Hz\n", g_tone_hz); }
void port_tone_volume(int percent) { (void)percent; }

/* The simulator has no codec, so there is nothing to hold */
void port_tone_hold(bool on) { (void)on; }
void port_boot_btn_fake(uint32_t ms) { (void)ms; }
uint32_t port_boot_isr_count(void) { return 0; }
bool port_tone_codec_open(void) { return false; }
int  port_hid_forget_all(void) { return 0; }
/* The simulator has no IMU. Only fake values, made with a finger. */
/* ── the fake IMU ────────────────────────────────────────────
 * 🚨 With no IMU in the simulator, everything driven by tilt (marble, the
 * brick tilt board, water gravity, the air mouse) sat outside every test.
 * That is why the bugs that only showed on the hardware clustered there
 * (09-09). Values can be pushed in from outside, and with nothing pushed it
 * answers "no IMU" as before — a device without one has to be imitable too. */
static float g_imu_x, g_imu_y, g_imu_z = 1000.0f;
static bool  g_imu_on;

void sim_imu_set(float x, float y, float z)
{ g_imu_x = x; g_imu_y = y; g_imu_z = z; g_imu_on = true; }
void sim_imu_off(void) { g_imu_on = false; }

/* The gyro is pushed in from outside the same way. With nothing pushed it says
 * "no gyro" and the air mouse falls back to tilt — that fork gets walked in the
 * simulator too. */
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
void        port_time_sync_start(void) { g_net = NET_SYNCED; }   /* the simulator uses the PC clock */
net_state_t port_time_sync_state(void) { return g_net; }
const char *port_bt_status(void) { return "off"; }

/* ── the BLE HID simulator stubs ──────────────────────────────
 * There is nowhere to really send, but printing the reports is enough to check
 * the gesture logic. */
static bool g_hid_on;
static int  g_hid_frames;

bool port_hid_ready(void) { return g_hid_on; }
bool port_hid_start(void) { g_hid_on = true;  g_hid_frames = 0; fprintf(stderr, "[hid] advertising\n"); return true; }
void port_hid_stop(void)  { g_hid_on = false; }
bool port_hid_connected(void) { return g_hid_on && ++g_hid_frames > 3; }  /* pretends to connect a moment later */
const char *port_hid_peer(void) { return port_hid_connected() ? "sim host" : "advertising"; }

/* The simulator has no peer. Sensitivity cannot be per-device and runs off one default. */
bool port_hid_peer_addr(uint8_t out[6]) { (void)out; return false; }

/* The simulator has no bonds. The screen needs a list to press, so a fake one is handed over. */
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

/* The simulator has no radio. The screen needs results to press, so fake ones are handed over.
 * 🚨 The same delay as the hardware (one second) — answering instantly would
 * never show the "scanning" screen, and a broken one would go unnoticed. */
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
    /* 🚨 The simulator has to be able to make the answer never arrive. The
     * device got stuck that way (09-13, the scan task could not be created),
     * and whether the screen finds its way out cannot be told without pressing
     * it. BADGE_SIM_SCAN_STALL=1 returns -1 for ever. */
    static int stall = -1;
    if (stall < 0) { const char *e = getenv("BADGE_SIM_SCAN_STALL"); stall = e && *e == '1'; }
    if (stall) return -1;
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
    /* In the simulator only names starting with sim- pretend to join — the failure screen has to be seen too. */
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

/* The simulator takes the finger count the browser counted (the N command) */
int g_touch_count = 1;
int port_touch_count(void) { return g_touch_count; }

/* The simulator shows it only as a black cover — on the hardware the pixels really go out */
void port_display_power(bool on) { (void)on; }

int port_pwr_key(void) { return 0; }   /* the simulator toggles directly with the W command */

uint32_t port_hid_passkey(void) { return 0; }

void port_rtc_restore(void) {}   /* the simulator uses the PC clock */
void port_time_autosync(void) {}
static int g_tz = 9 * 60;
void port_set_tz_offset(int m) { g_tz = m; }
int  port_get_tz_offset(void) { return g_tz; }

bool port_imu_angle(float *deg) { (void)deg; return false; }   /* the simulator has no sensor */
bool port_imu_upright(void) { return false; }
bool port_imu_accel(float *x, float *y)
{
    float z;
    return port_imu_accel3(x, y, &z);
}

int  port_battery_percent(void) { return 76; }   /* a plausible value for the simulator */
bool port_battery_charging(void) { return false; }
bool port_battery_plugged(void) { return false; }
int  port_battery_minutes_left(void) { return 5 * 60 + 20; }

void port_power_off(void) { fprintf(stderr, "[axp] power cut\n"); }

void port_heap_report(const char *when) { (void)when; }   /* the simulator has heap to spare */


/* ── the meeting button (simulator) ────────────────────────
 * The simulator has no phone, so it keeps a fake one. It answers a command a
 * moment later so the screen flow can be watched in the same order as on the
 * hardware. */
static uint32_t s_meet_due;    /* when the reply is due (us). 0 = none */
static uint8_t  s_meet_next;

bool port_meet_link(void) { return true; }

bool port_meet_send(uint8_t cmd)
{
    port_log("meet", "command 0x%02X (fake phone)", cmd);
    if (cmd == MEET_CMD_START_KO || cmd == MEET_CMD_START_EN) {
        s_meet_next = MEET_ACK_STARTED;
        s_meet_due  = port_micros() + 600000;      /* "microphone open" 0.6 s later */
    } else if (cmd == MEET_CMD_STOP) {
        s_meet_next = MEET_ACK_SAVED;
        s_meet_due  = port_micros() + 4000000;     /* "minutes saved" 4 s later */
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


/* ── recording (simulator) ────────────────────────────────
 * The simulator has neither microphone nor flash. It only goes through the
 * motions so the screen flow can be seen. */
static uint32_t s_rec_t0;
static bool     s_rec_on;
static int      s_rec_pend;

bool port_rec_start(int lang)
{
    (void)lang;
    port_log("rec", "recording started (simulator)");
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

/* The simulator has no BLE. It imitates what a phone usually gives. */
int port_hid_interval_ms(void) { return 15; }

void port_big_free(void *p) { free(p); }

void port_pm_hold(bool on) { (void)on; }
void port_perf_hold(bool on) { (void)on; }   /* the simulator has one speed */

void port_battery_journal_dump(void) {}

void port_reset_reason_note(int rr) { (void)rr; }

static int s_crumb_sim = -1;
void port_crumb(int what) { s_crumb_sim = what; }
int  port_crumb_now(void) { return s_crumb_sim; }
void port_uptime_mark(void) {}

/* The simulator has no panel. It imitates just enough for the settings screen to work. */
static int s_sim_xgap = 6;
void badge_display_set_xgap(int g) { s_sim_xgap = g; }
int  badge_display_get_xgap(void)  { return s_sim_xgap; }

/* The simulator makes its own LVGL input device. Used when the launcher asks, for power saving. */
lv_indev_t *badge_display_indev(void) { return lv_indev_get_next(NULL); }

void port_cpu_mark(void) {}
void port_cpu_report(const char *w) { (void)w; }

void port_imu_idle_check(void) {}

void port_health_begin(void) {}
int  port_health_check(char *o, size_t n) { if (n) o[0] = 0; return 0; }
uint32_t port_health_errors(void) { return 0; }

/* ── pretending to export over USB ──────────────────────────
 * 🚨 The simulator has no USB. It fakes the flow (off → waiting → attached →
 * ejected) so the screens can be seen. The side that builds the volume
 * (usb_export.c) reads the recording partition and is left out of the
 * simulator — the count is made up here. */
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
