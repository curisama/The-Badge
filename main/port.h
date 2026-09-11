#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* A thin layer so the board (ESP32) and the PC simulator can share the same
 * UI code. Anything an app calls that is not here will not run in the
 * simulator. */

void port_lock(void);       /* the LVGL mutex */
void port_unlock(void);
void port_log(const char *tag, const char *fmt, ...);

bool port_kv_read(const char *key, void *out, size_t len);
void port_kv_write(const char *key, const void *in, size_t len);

void port_home_button_start(void (*on_press)(void));

/* Radio state changes. In the simulator this only logs. */
void port_radio_set(int need);

/* Microsecond clock and delay.
 * In the simulator the clock is virtual, so this advances time rather than waiting. */
uint32_t port_micros(void);
void     port_delay_us(uint32_t us);

/* Large buffers (canvases and the like). On the board these come from PSRAM. */
void *port_big_alloc(size_t n);
void  port_big_free(void *p);
void  port_heap_report(const char *when);   /* log the heap state */

/* Battery level (%). -1 if there is no battery or it cannot be read. */
void port_power_off(void);   /* tell the AXP2101 to shut down */
int port_battery_percent(void);
bool port_battery_charging(void);
bool port_battery_plugged(void);   /* is USB connected? (true even when charging has finished) */
/* Minutes left at the current rate; -1 if not measured yet. */
int  port_battery_minutes_left(void);

/* Which way gravity points within the screen plane, in degrees — so "down" is
 * known however the badge is held. Lying flat there is no in-plane component,
 * and it returns false. */
bool port_imu_angle(float *deg);
bool port_imu_upright(void);
/* Sleeps the accelerometer when unused. The launcher calls this periodically. */
void port_imu_idle_check(void);
/* Gravity in the screen plane (mg). Used to roll the marble. */
bool port_imu_accel(float *x, float *y);   /* is the screen upright? (false when flat) */
bool port_imu_accel3(float *x, float *y, float *z); /* all three axes (mg), so shaking is visible too */

/* Angular rate in degrees per second. The axes are the accelerometer's frame —
 * rotation about x is gx.
 *
 * 🚨 The accelerometer only knows *attitude*. Carry the badge across the room
 * without changing how it is held and nothing happens. A gyro gives *how far
 * it turned*, which is what makes a real air mouse.
 * 🔋 But a gyro costs more than ten times the accelerometer (roughly 1.5 mA
 * against 0.03). So it is off by default: whoever wants it switches it on and
 * off again on the way out. Left on, it keeps running with the display off —
 * exactly what happened with the accelerometer once. */
void port_imu_gyro_enable(bool on);
bool port_imu_gyro(float *x, float *y, float *z);

/* How many fingers are on the screen (0-2). The CST9217 reports two.
 * LVGL only passes one through, so two-finger gestures are decided from this. */
int port_touch_count(void);

/* ── BLE HID mouse ──────────────────────────────────────────
 * This badge is the peripheral. Being standard HID, phones, PCs and TVs take
 * it with no driver. */
void        port_hid_start(void);      /* bring the stack up and start advertising */
void        port_hid_stop(void);       /* stop advertising only; keep the connection */
bool        port_hid_connected(void);
bool        port_hid_up(void);          /* is the stack up? (separate from being connected) */
int         port_hid_forget_all(void); /* forget every paired device */

/* ── hosts this badge has been connected to ──────────────────
 * 🚨 A BLE bond stores **only the address**. The host's name never arrives —
 * the badge is the peripheral and has no reason to see one. So a list looks
 * like three rows of A4:83:E7:... with no way to tell which is the PC at
 * home. Now that there is a keypad, a person names them
 * (port_hid_host_name_set). */
#define HID_HOSTS_MAX 8
typedef struct {
    uint8_t addr[6];
    char    name[17];      /* the name a person gave it; empty shows the address */
    bool    here;          /* is this the one currently connected? */
} hid_host_t;
int  port_hid_hosts(hid_host_t *out, int max);
void port_hid_host_name_set(const uint8_t addr[6], const char *name);
/* 🚨 Advertise to that host only, and drop the current link.
 * If anything other than the chosen host connects, it is dropped on the spot —
 * a whitelist alone leaks depending on the stack. */
void port_hid_host_pick(const uint8_t addr[6]);
void port_hid_host_any(void);          /* accept anything (the default) */
const char *port_hid_peer(void);       /* the connected host, for display */
void        port_hid_mouse(int dx, int dy, unsigned buttons, int wheel);
/* The address (6 bytes) of whatever is connected now, used to remember
 * per-device settings.
 * 🚨 ESP_GAP_BLE_AUTH_CMPL_EVT also fires on reconnect, so this stays current. */
bool        port_hid_peer_addr(uint8_t out[6]);

/* ── scanning for WiFi (used by the UI) ───────────────────────
 * 🚨 A scan takes a second or two and the display must not freeze, so it runs
 * **in a task and the UI polls it**. Reporting through a callback would mean
 * touching LVGL from another thread, which LVGL does not tolerate.
 * 🚨 It brings WiFi up and down, so it must not overlap BLE — the calling
 * app's radio has to be RADIO_OFF. */
#define WIFI_SCAN_MAX 16
typedef struct { char ssid[33]; int8_t rssi; uint8_t saved; } wifi_found_t;
void        port_wifi_scan_start(void);
/* -1 = still scanning, >= 0 = how many were found */
int         port_wifi_scan_result(wifi_found_t *out, int max);

/* Write and clear slots (0-2). Called from the UI. */
#define WIFI_SLOTS 3
void        port_wifi_slot_set(int slot, const char *ssid, const char *pass);
void        port_wifi_slot_clear(int slot);
/* 🚨 The password is never returned — only whether there is one. */
bool        port_wifi_slot_get(int slot, char *ssid, size_t ss);
/* Which slot holds this SSID, or -1 */
int         port_wifi_slot_find(const char *ssid);
/* A free slot. When full, returns the last one (pushing out the oldest) */
int         port_wifi_slot_free(void);

/* ── actually trying to join ──────────────────────────────────
 * 🚨 Storing credentials without trying them leaves no way to know the
 * password was wrong. It joins once immediately and shows the result.
 * 🚨 The badge does **not stay on WiFi** — it joins to set the clock or to
 * upload and leaves again, for power. So "connected" does not mean connected
 * right now; it means **the last network that worked**, which is the question
 * a person actually has ("is this one good?"). */
void        port_wifi_try(const char *ssid, const char *pass);
#define WIFI_TRY_BUSY (-1)
#define WIFI_TRY_FAIL  0
#define WIFI_TRY_OK    1
int         port_wifi_try_state(void);
/* The last network that worked, or an empty string */
void        port_wifi_last_ok(char *ssid, size_t ss);
/* Press and release one key. Modifier bits: 1=Ctrl 2=Shift 4=Alt 8=GUI */
void        port_hid_key(unsigned modifier, unsigned keycode);
/* Type an ASCII string on the keyboard. Characters it cannot produce are skipped. */
void        port_hid_type(const char *s);
uint32_t    port_hid_passkey(void);
/* The connection interval the host granted (ms). Reports sent faster than this queue up. */
int         port_hid_interval_ms(void);   /* the pairing code, or 0 if there is none */


/* ── the meeting button ─────────────────────────────────────
 * The badge only sends the signal; the recording is done by an app on the
 * phone, so the minutes survive the badge's battery dying. Sending a command
 * must not change the screen — it goes to "recording" only once the app has
 * replied that the microphone is open. */
#define MEET_CMD_START_KO 0x01   /* badge -> app */
#define MEET_CMD_START_EN 0x11
#define MEET_CMD_STOP     0x02
#define MEET_ACK_STARTED  0x81   /* app -> badge */
#define MEET_ACK_SAVED    0xC0
#define MEET_ACK_FAIL     0xE1

bool port_meet_link(void);            /* is the app connected and subscribed? */
bool port_meet_send(uint8_t cmd);     /* false if it could not be sent */
bool port_meet_recv(uint8_t *status); /* true, and fills status, if a reply arrived */

/* ── recording ──────────────────────────────────────────────
 * The badge opens the microphone itself and writes to flash. No phone and no
 * WiFi. Getting it off is a separate matter later, so nothing is lost when a
 * connection drops mid-meeting. */
size_t   port_rec_capacity(void);      /* size of the recording partition; 0 = none */
bool     port_rec_start(int lang);     /* false on failure */
void     port_rec_stop(void);
bool     port_rec_active(void);
uint32_t port_rec_seconds(void);       /* length of the current recording (s) */
uint32_t port_rec_free_seconds(void);  /* how many more seconds will fit */
int      port_rec_pending(void);       /* recordings still on the device */

/* Battery voltage (mV). The AXP2101 gives voltage but not current. */
int  port_battery_mv(void);
/* Log voltage and level once a minute, to build the real discharge curve. */
void port_battery_log(const char *what);
/* Called where the display is switched on and just before it goes off. The
 * gap between those two is "the display was on", which is what makes its cost
 * measurable. 🚨 Called after switching off, it records display-off and the
 * pair is broken. */
void port_battery_mark(bool screen_on);
/* Dump the journal into the boot log when a cable is plugged in. */
void port_battery_journal_dump(void);
/* What fraction of the time the CPU worked. mark() sets the start, report()
 * examines that interval. It is the only measure of power saving that works
 * while plugged in. */
/* Health check — not "did it crash" but "do the things a person sees work".
 * begin() starts counting, and a non-zero check() means something is wrong. */
void     port_health_begin(void);
int      port_health_check(char *out, size_t len);
uint32_t port_health_errors(void);

void port_cpu_mark(void);
void port_cpu_report(const char *when);
/* Count reboot reasons in NVS. Brownout versus panic is a question of totals. */
void port_reset_reason_note(int rr);

/* Note what is happening now. After a reboot this becomes "what it was doing
 * when it died". Call it on transitions only — every frame would wear the flash. */
enum { CRUMB_BOOT, CRUMB_HOME, CRUMB_LOCK, CRUMB_SCR_OFF, CRUMB_SCR_ON,
       CRUMB_APP_OPEN, CRUMB_MOUSE, CRUMB_MEET, CRUMB_GAME, CRUMB_CALC,
       CRUMB_CLOCK, CRUMB_SETTINGS, CRUMB_REC };
void port_crumb(int what);
/* What is currently being counted, used to rejoin an app's stretch after unlocking. */
int  port_crumb_now(void);
void port_uptime_mark(void);   /* every 60 s; becomes the uptime on the next boot */

/* Sound. Switching one frequency on and off is all this needs. */
void port_tone_init(void);
void port_tone_freq(uint32_t hz);
void port_tone_enable(bool on);
void port_tone_volume(int percent);
/* An app that makes frequent sounds calls hold(true) on entry and hold(false)
 * on exit, so the codec is already open and the first sound is not lost. */
void port_tone_hold(bool on);

/* For tests — press BOOT from software. GPIO0 is a strapping pin, so keep it
 * brief and only while the display is on. */
void     port_boot_btn_fake(uint32_t ms);
uint32_t port_boot_isr_count(void);
bool     port_tone_codec_open(void);   /* is the codec actually claimed? */

/* Setting the clock. There is no room to type a password on a 1.75-inch round
 * screen, so rather than showing a network list this joins, sets the clock and
 * leaves. */
typedef enum {
    NET_NOCONF = 0,   /* no credentials */
    NET_IDLE,
    NET_CONNECTING,
    NET_SYNCED,
    NET_FAIL,
} net_state_t;

/* The WiFi netif can only be created once; the clock and the upload share it. */
struct esp_netif_obj;
struct esp_netif_obj *badge_wifi_netif_once(void);
/* One WiFi user at a time — the clock and an upload overlapping would bring
 * the hardware up twice. */
/* Credentials live on the badge (NVS). Values in secrets.h are written at
 * flash time; with it empty, what is stored is used — so flashing from
 * another machine does not lose them. */
void badge_creds_init(void);
bool badge_creds_wifi(char *ssid, size_t ss, char *pass, size_t ps);
/* Call after WiFi is up — with our slots empty, this takes what the stack stored */
bool badge_creds_wifi_live(char *ssid, size_t ss, char *pass, size_t ps);
/* Three slots (home, a hotspot, one spare). Is any of them filled? */
bool badge_creds_wifi_any(void);
/* 🚨 Call **after** WiFi is up. One scan picks whichever stored network is in
 * range with the strongest signal — which avoids burning 5-10 seconds per
 * attempt on networks that are not there. */
bool badge_wifi_pick(char *ssid, size_t ss, char *pass, size_t ps);

bool badge_wifi_take(uint32_t wait_ms);
void badge_wifi_give(void);

void        port_time_sync_start(void);
void        port_rtc_restore(void);    /* apply the stored time zone */
void        port_time_autosync(void);  /* set the clock once at boot if it is unset */
void        port_set_tz_offset(int minutes);
int         port_get_tz_offset(void);
net_state_t port_time_sync_state(void);
const char *port_bt_status(void);

/* The PWR button. Not a GPIO — the AXP2101's PWRON pin, read over I2C.
 * 0 = nothing, 1 = short, 2 = long (just before the hardware cuts power) */
int port_pwr_key(void);

/* Switch the panel itself off and on. On an AMOLED a dark pixel costs
 * nothing, so turning it off is very different from dimming it. */
void port_display_power(bool on);

/* Display brightness 0-100. On an AMOLED brightness is battery. */
void port_brightness_set(int percent);
int  port_brightness_get(void);

/* Block light sleep briefly. Held only while doing something that breaks when
 * the CPU sleeps, such as a BLE connection or I2S recording. Nestable. */
void port_pm_hold(bool on);

/* The background loop. In the simulator this does nothing and the caller drives it. */
void port_task_start(const char *name, void (*fn)(void *), void *arg, int stack);
