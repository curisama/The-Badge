#include "port.h"
#include "app.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "display.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdarg.h>

/* BOOT is GPIO0 and can be used as an ordinary button at runtime.
 * PWR is not a GPIO at all — it is the AXP2101's PWRON pin — so it cannot be
 * read here. */
#define HOME_BTN_GPIO   GPIO_NUM_0

static volatile bool s_boot_hit;    /* press flag set by the interrupt */
/* It is a level interrupt, so it fires continuously while held. It is masked
 * the moment it is caught; the task below re-arms it after the finger lifts. */
static volatile uint32_t s_boot_isr_n;   /* how many times the interrupt actually fired */
static void IRAM_ATTR boot_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(HOME_BTN_GPIO);
    s_boot_isr_n++;
    s_boot_hit = true;
}

uint32_t port_boot_isr_count(void) { return s_boot_isr_n; }

/* 🚨 Press BOOT (GPIO0) from software. The pin is open drain, so code can
 * only pull it LOW and never drive it HIGH — a person pressing at the same
 * time cannot fight it.
 * ⚠️ GPIO0 is a boot-mode strapping pin. A reset while it is held low enters
 *    download mode. So it is pulsed briefly, and only while the display is on
 *    and light sleep is therefore not engaged. Nothing but tests calls it. */
void port_boot_btn_fake(uint32_t ms)
{
    /* 🚨 Do not use gpio_config() here — it resets that pin's interrupt
     * configuration as well, so the test tool switches off the thing it is
     * testing (it really happened: the first press was caught by the
     * interrupt and every press after that read zero). Change the direction
     * only. */
    gpio_set_direction(HOME_BTN_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(HOME_BTN_GPIO, 0);            /* press */
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(HOME_BTN_GPIO, 1);            /* release (the pull-up raises it) */
    gpio_set_direction(HOME_BTN_GPIO, GPIO_MODE_INPUT);
}
#define KV_NS           "badge"

void port_lock(void)
{
    /* 0 means "fail immediately rather than wait". If the LVGL task holds it,
     * that means drawing without the lock — which showed up as an error in the
     * boot log. Wait instead. */
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        ESP_LOGW("port", "failed to take the LVGL lock");
    }
}
void port_unlock(void) { bsp_display_unlock(); }

void port_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    esp_log_writev(ESP_LOG_INFO, tag, fmt, ap);
    va_end(ap);
    esp_log_write(ESP_LOG_INFO, tag, "\n");
}

bool port_kv_read(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(KV_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = len;
    esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK && sz == len;
}

void port_kv_write(const char *key, const void *in, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(KV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, in, len);
    nvs_commit(h);
    nvs_close(h);
}

static void (*s_on_press)(void);
static void (*s_on_hold)(void);
static void axp_init(void);
static void batt_track(int pct, bool plugged);
/* Last brightness set. The BSP does not remember it and goes to 100% on every power-on. */
static int s_bright_saved = 45;

static void rtc_write_now(void);
static void time_synced_now(void);

/* Called only when SNTP actually set the clock. This flag is the only proof of success. */
static volatile bool s_sntp_done;
static void sntp_got_time(struct timeval *tv) { (void)tv; s_sntp_done = true; }

/* ── the light-sleep lock ─────────────────────────────────────
 * With light sleep on, the CPU sleeps when there is nothing to do and the
 * standby current drops a long way. But a BLE connection and I2S recording
 * can both break while it sleeps, so those two hold it awake while they run.
 * Several things can hold it, so it is counted and released by the last one. */
#include "esp_pm.h"
static esp_pm_lock_handle_t s_nosleep;
static int                  s_nosleep_n;

void port_pm_hold(bool on)
{
    if (!s_nosleep) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "badge", &s_nosleep) != ESP_OK) return;
    }
    if (on) {
        if (s_nosleep_n++ == 0) esp_pm_lock_acquire(s_nosleep);
    } else if (s_nosleep_n > 0) {
        if (--s_nosleep_n == 0) esp_pm_lock_release(s_nosleep);
    }
}

/* ── the top-speed lock ──────────────────────────────────
 * 🚨 Keeping the chip awake and running it fast are **two different locks.**
 * NO_LIGHT_SLEEP above only stops it sleeping; it does not take a waking CPU
 * from 80 MHz to 240. Dynamic frequency scaling parks at min_freq whenever
 * nobody holds a CPU_FREQ_MAX lock, and it never raises the clock because the
 * work looks heavy. So an app where the arithmetic *is* the frame — the water
 * — has been running at a third of the chip's speed all along.
 *
 * Held only while such an app is open. Holding it always would just burn
 * battery. Nesting is fine. */
static esp_pm_lock_handle_t s_fast;
static int                  s_fast_n;

void port_perf_hold(bool on)
{
    if (!s_fast) {
        if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "badge-fast", &s_fast) != ESP_OK) return;
    }
    if (on) {
        if (s_fast_n++ == 0) esp_pm_lock_acquire(s_fast);
    } else if (s_fast_n > 0) {
        if (--s_fast_n == 0) esp_pm_lock_release(s_fast);
    }
}

static void home_btn_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HOME_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* 🚨 With the display off this was only polled every 300 ms. A tap takes
     * 100-200 ms, so the whole press could fall between two polls and be
     * invisible.
     * On top of that the CPU is in light sleep in between, so an interrupt
     * alone cannot wake it either. Waking from light sleep requires a *level*
     * trigger, not an edge — so the press (LOW) is the wake condition. */
    /* 🚨 If something already installed it, IDF logs an E and returns
     * INVALID_STATE. That is a normal case for us, but it leaves an error in
     * the log that gets in the way when hunting a real one. Called quietly,
     * once. */
    esp_log_level_t lv = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);
    esp_err_t ie = gpio_install_isr_service(0);
    esp_log_level_set("gpio", lv);
    if (ie != ESP_OK && ie != ESP_ERR_INVALID_STATE)
        ESP_LOGW("btn", "interrupt service failed: %s — BOOT will only be polled", esp_err_to_name(ie));
    gpio_isr_handler_add(HOME_BTN_GPIO, boot_isr, NULL);
    gpio_wakeup_enable(HOME_BTN_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    gpio_intr_enable(HOME_BTN_GPIO);

    /* Short press = home, held for a second = display off */
    axp_init();

    /* 🚨 Polling every 300 ms with the display off was the problem: a tap
     * takes 100-200 ms and could fall entirely between two polls.
     * Polling everything faster is not the answer either — the expensive part
     * is not the GPIO, it is the AXP over I2C. So they are split: the pin
     * every 100 ms (free), the AXP every 300 ms (unchanged). The interrupt
     * usually gets there first, but even when it misses, 100 ms does not. */
    const int SLICE_ON = 40, SLICE_OFF = 100;
    int prev = 1, held = 0, axp_acc = 0;
    bool fired = false;
    while (1) {
        bool off = launcher_screen_is_off();
        int slice = off ? SLICE_OFF : SLICE_ON;

        axp_acc += slice;
        if (!off || axp_acc >= 300) {
            axp_acc = 0;
            /* PWR short = home. A power button that does nothing is a button nobody finds. */
            int pk = port_pwr_key();
            if (pk == 1) {
                port_lock(); launcher_home(); port_unlock();
            } else if (pk == 2) {
            /* Show the notice for a moment, then actually cut power */
                port_lock(); launcher_poweroff_notice(); port_unlock();
                vTaskDelay(pdMS_TO_TICKS(700));
                port_power_off();
            }
        }

        if (s_boot_hit) {
            s_boot_hit = false;
            /* Log whether the pin was what woke it. This situation cannot be
             * produced in software (the CPU would be the one pressing), so it
             * is only ever confirmed by a real press — hence the note. */
            esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
            ESP_LOGI("btn", "BOOT pressed [interrupt] (last wake source: %s)",
                     wc == ESP_SLEEP_WAKEUP_GPIO  ? "pin"
                   : wc == ESP_SLEEP_WAKEUP_TIMER ? "timer"
                   : wc == ESP_SLEEP_WAKEUP_UNDEFINED ? "did not sleep" : "other");
            if (s_on_press) { port_lock(); s_on_press(); port_unlock(); }
            /* Wait for the finger to lift before re-arming. Re-arming while
             * it is still held retriggers the level interrupt endlessly. */
            for (int i = 0; i < 60 && gpio_get_level(HOME_BTN_GPIO) == 0; i++)
                vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(60));       /* debounce */
            gpio_intr_enable(HOME_BTN_GPIO);
            held = 0; fired = false; prev = 1;
            continue;
        }

        int now = gpio_get_level(HOME_BTN_GPIO);
        if (now == 0) {
            held += slice;
            if (held >= 1000 && !fired && s_on_hold) {
                fired = true;
                port_lock(); s_on_hold(); port_unlock();
            }
        } else {
            /* The polling path, for when the interrupt missed it */
            if (prev == 0 && !fired && s_on_press) {
                ESP_LOGI("btn", "BOOT pressed [polled] — the interrupt missed it");
                port_lock(); s_on_press(); port_unlock();
            }
            held = 0;
            fired = false;
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(slice));
    }
}

void port_home_button_start(void (*on_press)(void))
{
    /* Worn upside down, BOOT is where a finger naturally lands, so display
     * on/off moved here. PWR short = home; PWR held = power cut, which is
     * hardware and cannot be changed. */
    (void)on_press;
    s_on_press = launcher_screen_toggle;
    s_on_hold  = NULL;
    xTaskCreate(home_btn_task, "home_btn", 3072, NULL, 4, NULL);
}

void port_radio_set(int need)
{
    /* TODO: BLE HID on/off. It shares the antenna with WiFi, so never both at once. */
    ESP_LOGI("radio", "need=%d", need);
}

/* ── emulator support ─────────────────────────────────────────── */
#include "esp_timer.h"
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "rom/ets_sys.h"

uint32_t port_micros(void) { return (uint32_t)esp_timer_get_time(); }

void port_delay_us(uint32_t us)
{
    /* Yield the task for anything over a millisecond; spin for less.
     * At 32768 Hz a cycle is 30 us, which is not worth sleeping for. */
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us) esp_rom_delay_us(us);
}

/* Guessing whether a crash was memory is no way to settle it. Write the
 * numbers down. Internal RAM is only 512 KB and the BLE stack takes a large
 * share of it; PSRAM is 8 MB and roomy. When the largest free block falls far
 * below the total, that is fragmentation. */
void port_heap_report(const char *when)
{
    size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t i_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("heap", "%-10s internal %uKB (largest block %uKB, low water %uKB) PSRAM %uKB",
             when, (unsigned)(i_free / 1024), (unsigned)(i_big / 1024),
             (unsigned)(i_min / 1024), (unsigned)(p_free / 1024));

    if (i_free < 24 * 1024) ESP_LOGW("heap", "internal RAM is running out");
}

void *port_big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

void port_big_free(void *p) { free(p); }

void port_task_start(const char *name, void (*fn)(void *), void *arg, int stack)
{
    xTaskCreate(fn, name, stack, arg, 5, NULL);
}

/* ── sound ───────────────────────────────────────────────────
 * A square wave at a given frequency is all this needs. It is pushed into the
 * ES8311 codec over I2S. */
#include "esp_codec_dev.h"

#define TONE_SR     16000
#define TONE_CHUNK  256

static esp_codec_dev_handle_t s_spk;
static int64_t s_tone_last_us;
static bool    s_codec_open;
static bool    tone_muted(void);
static void    codec_open(void);
static void    codec_close(void);
static volatile uint32_t s_tone_hz = 1000;
static volatile bool     s_tone_on;
static int               s_tone_vol = 60;
static volatile bool     s_tone_held;   /* is an app that uses sound open? */

static void tone_task(void *arg)
{
    (void)arg;
    static int16_t buf[TONE_CHUNK];
    uint32_t phase = 0;

    while (1) {
        if (!s_tone_on || !s_spk) {
            phase = 0;
            /* 🚨 Opening the codec for every sound loses the first one
             * entirely (on the board: after a power-saving pause, a game's
             * first effect was missing and only later ones played). Holding
             * it open for as long as a sound-using app is open breaks the
             * power saving instead. While held, only the threshold is
             * extended — twenty seconds of silence is not idling, so it
             * closes then. */
            int64_t idle = s_tone_held ? 20000000 : 3000000;
            if (s_codec_open && (tone_muted() || esp_timer_get_time() - s_tone_last_us > idle))
                codec_close();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t hz = s_tone_hz ? s_tone_hz : 1000;
        uint32_t period = TONE_SR / hz;          /* samples in one cycle */
        if (period < 2) period = 2;
        int16_t amp = (int16_t)(9000 * s_tone_vol / 100);

        for (int i = 0; i < TONE_CHUNK; i++) {
            buf[i] = (phase < period / 2) ? amp : -amp;
            if (++phase >= period) phase = 0;
        }
        esp_err_t we = esp_codec_dev_write(s_spk, buf, sizeof(buf));
    if (we != ESP_OK) { static int c; if (c++ % 50 == 0) ESP_LOGE("tone", "write failed: %s", esp_err_to_name(we)); }
    }
}

void port_tone_init(void)
{
    if (s_spk) return;
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGW("tone", "speaker init failed — carrying on without sound");
        return;
    }
    xTaskCreate(tone_task, "tone", 3072, NULL, 6, NULL);   /* the codec is opened when needed */
}

void port_tone_freq(uint32_t hz)  { s_tone_hz = hz; }
/* An open codec holds its I2S buffers. Three seconds without a sound closes
 * it, and it reopens when needed. How long opening takes goes in the log. */
static void codec_open(void)
{
    if (s_codec_open || !s_spk) return;
    int64_t t0 = esp_timer_get_time();
    esp_codec_dev_sample_info_t fs = { .sample_rate = TONE_SR, .channel = 1, .bits_per_sample = 16 };
    esp_err_t oe = esp_codec_dev_open(s_spk, &fs);
    if (oe != ESP_OK) {
        /* 🚨 Recording a successful open after a failure means later trying
         * to disable a channel that was never enabled, which produces
         * i2s_channel_disable errors (fifteen of them in the self-test). */
        ESP_LOGW("tone", "speaker open failed: %s — recording has the I2S. Skipping sound only", esp_err_to_name(oe));
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, s_tone_vol);
    s_codec_open = true;
    ESP_LOGI("tone", "codec opened in %lld ms", (esp_timer_get_time() - t0) / 1000);
}

static void codec_close(void)
{
    if (!s_codec_open || !s_spk) return;
    esp_codec_dev_close(s_spk);
    s_codec_open = false;
}

void port_tone_enable(bool on)
{
    /* 🚨 Nothing used to call port_tone_init(), so s_spk was NULL,
     * codec_open() returned quietly, and no sound ever came out at all. The
     * init call appears to have been deleted along with an app that was
     * removed. It is brought up on the first sound now — unused, the codec is
     * never claimed. */
    /* 🚨 Stops the codec being claimed even while muted. */
    if (on && tone_muted()) { s_tone_on = false; return; }
    if (on && !s_spk) port_tone_init();
    if (on) { codec_open(); s_tone_last_us = esp_timer_get_time(); }
    s_tone_on = on;
}
/* Muting used to set the volume to zero and leave the codec open, pushing
 * silence with I2S running and the amplifier on — spending power to produce
 * something nobody can hear. Volume 0 now means not touching it at all. */
static bool tone_muted(void) { return s_tone_vol <= 0; }

bool port_tone_codec_open(void) { return s_codec_open; }

void port_tone_hold(bool on)
{
    s_tone_held = on && !tone_muted();
    if (s_tone_held) {
        if (!s_spk) port_tone_init();
        codec_open();
        s_tone_last_us = esp_timer_get_time();
    }
}

void port_tone_volume(int percent)
{
    s_tone_vol = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (tone_muted()) {
        /* Release what is already open at the moment of muting. No reason to wait for the next sound. */
        s_tone_on = false;
        s_tone_held = false;
        return;                 /* the tone task does the closing, so only one place touches it */
    }
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_tone_vol);
}

void port_brightness_set(int percent)
{
    if (percent < 1) percent = 1;      /* 0 means "off", which Settings must not be able to reach */
    if (percent > 100) percent = 100;
    s_bright_saved = percent;
    badge_display_brightness(percent);
}
int  port_brightness_get(void)        { return s_bright_saved; }

/* ── setting the clock ────────────────────────────────────────
 * Bring WiFi up, set the clock over SNTP, take it down again. There is no
 * reason to stay connected, and WiFi shares the antenna with BLE, so leaving
 * it on makes the mouse sluggish. */
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#if __has_include("secrets.h")
#   include "secrets.h"
#endif
#ifndef BADGE_WIFI_SSID
#   define BADGE_WIFI_SSID ""
#endif
#ifndef BADGE_WIFI_PASS
#   define BADGE_WIFI_PASS ""
#endif
/* Add more here to cover more places. A slot with nothing in it is treated as absent. */
#ifndef BADGE_WIFI_SSID2
#   define BADGE_WIFI_SSID2 ""
#endif
#ifndef BADGE_WIFI_PASS2
#   define BADGE_WIFI_PASS2 ""
#endif
#ifndef BADGE_WIFI_SSID3
#   define BADGE_WIFI_SSID3 ""
#endif
#ifndef BADGE_WIFI_PASS3
#   define BADGE_WIFI_PASS3 ""
#endif

/* ── credentials live on the badge ───────────────────────────
 * 🚨 Hard-coding them means a badge flashed from a machine without secrets.h
 * (the one at work, say) loses WiFi entirely. Worse, an older example file
 * had placeholder text in it, and calling esp_wifi_set_config with that
 * **overwrote the perfectly good credentials stored on the badge**.
 *
 * NVS survives reflashing, so they are written once and read from there.
 * With values in secrets.h they are seeded at flash time (flashing from
 * home); with it empty, whatever is stored is used as-is (flashing from
 * anywhere else). */
#define WIFI_NS  "badge"
/* WIFI_SLOTS is defined in port.h — the UI has to see the same number */

/* 🚨 The values from secrets.h used to overwrite what was on the badge **on
 * every boot**. With no way to enter WiFi on the device that was right, but
 * once there was a keypad, anything typed on screen reverted to secrets.h on
 * the next reboot ("I don't think it actually connected"). **Only fill empty
 * slots.**
 * The original purpose still holds: a brand-new badge gets the seed on its
 * first flash, and flashing from a machine with an empty secrets.h does not
 * erase what the badge already had.
 * 🚨 To push a new value from secrets.h, clear that slot on the device first,
 * then flash. */
static void creds_seed(const char *key, const char *val)
{
    if (!val || !val[0]) return;             /* empty means leave it alone */
    nvs_handle_t nh;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &nh) != ESP_OK) return;
    char old[64] = "";
    size_t n = sizeof old;
    if (nvs_get_str(nh, key, old, &n) == ESP_OK && old[0]) {
        nvs_close(nh);                       /* already set — do not touch */
        return;
    }
    nvs_set_str(nh, key, val);
    nvs_commit(nh);
    ESP_LOGI("wifi", "seeded %s onto the badge", key);
    nvs_close(nh);
}

/* Returns the stored value, or an empty string. */
static void creds_get(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t nh;
    if (nvs_open(WIFI_NS, NVS_READONLY, &nh) != ESP_OK) return;
    size_t n = cap;
    if (nvs_get_str(nh, key, out, &n) != ESP_OK) out[0] = '\0';
    nvs_close(nh);
}

/* 🚨 Slot 1 keeps the old key name ("wifi_ssid"). Renaming it would lose the
 * network already stored on the badge — the whole point of NVS is that it
 * survives reflashing, and changing the key throws that away. */
static void slot_key(char *out, size_t cap, const char *base, int i)
{
    if (i == 0) snprintf(out, cap, "%s", base);
    else        snprintf(out, cap, "%s%d", base, i + 1);
}

static bool creds_wifi_slot(int i, char *ssid, size_t ss, char *pass, size_t ps)
{
    char k[24];
    slot_key(k, sizeof k, "wifi_ssid", i); creds_get(k, ssid, ss);
    slot_key(k, sizeof k, "wifi_pass", i); creds_get(k, pass, ps);
    return ssid[0] != '\0';
}

void badge_creds_init(void)
{
    creds_seed("wifi_ssid",  BADGE_WIFI_SSID);
    creds_seed("wifi_pass",  BADGE_WIFI_PASS);
    creds_seed("wifi_ssid2", BADGE_WIFI_SSID2);
    creds_seed("wifi_pass2", BADGE_WIFI_PASS2);
    creds_seed("wifi_ssid3", BADGE_WIFI_SSID3);
    creds_seed("wifi_pass3", BADGE_WIFI_PASS3);
}

/* Is any slot filled in? */
bool badge_creds_wifi_any(void)
{
    char s2[33], p2[65];
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (creds_wifi_slot(i, s2, sizeof s2, p2, sizeof p2)) return true;
    return false;
}

bool badge_creds_wifi(char *ssid, size_t ss, char *pass, size_t ps)
{
    creds_get("wifi_ssid", ssid, ss);
    creds_get("wifi_pass", pass, ps);
    return ssid[0] != '\0';
}


/* 🚨 Call this **after** WiFi is up. When our slots are empty, whatever the
 * WiFi stack saved last time is used — which removes the ordering dependency
 * of "you can only flash at work after seeding it once at home". Anything
 * recovered is written into our slots too.
 * ⏳ Whether the stack really hands back what it stored has **not been tested
 *    on the board**. If it does not, the log says there was nothing to take. */
/* Pick whichever stored network is **actually in range right now**.
 *
 * 🚨 Trying them in order was deliberately not done. Failing to join a
 * network that is not there costs 5-10 seconds each time, so being away from
 * home would mean waiting for the home network first, every time. One scan
 * takes a second or two and wastes no attempts, which makes it faster in
 * practice.
 *
 * 🚨 Scanning requires WiFi to be up already, so this must be called after
 * esp_wifi_start — which is why choosing credentials moved to **after** the
 * start. It used to happen before. */
/* ── the scan the UI calls ─────────────────────────────────── */
static wifi_found_t  s_scan[WIFI_SCAN_MAX];
static volatile int  s_scan_n = -1;      /* -1 = still scanning */
static volatile bool s_scan_busy;

static void scan_task(void *arg)
{
    (void)arg;
    int found = 0;
    if (badge_wifi_take(8000)) {
        esp_netif_t *nif = badge_wifi_netif_once();
        (void)nif;
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&ic) == ESP_OK) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            if (esp_wifi_start() == ESP_OK) {
                wifi_scan_config_t sc = { 0 };
                if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                    uint16_t got = 0;
                    esp_wifi_scan_get_ap_num(&got);
                    uint16_t want = got > WIFI_SCAN_MAX ? WIFI_SCAN_MAX : got;
                    wifi_ap_record_t *ap = want ? calloc(want, sizeof *ap) : NULL;
                    if (ap) {
                        esp_wifi_scan_get_ap_records(&want, ap);
                        for (int i = 0; i < want; i++) {
                            if (!ap[i].ssid[0]) continue;   /* a hidden network cannot be chosen */
                            snprintf(s_scan[found].ssid, sizeof s_scan[found].ssid,
                                     "%s", (const char *)ap[i].ssid);
                            s_scan[found].rssi = ap[i].rssi;
                            /* Mark the ones already stored */
                            s_scan[found].saved = 0;
                            for (int b = 0; b < WIFI_SLOTS; b++) {
                                char s2[33], p2[65];
                                if (creds_wifi_slot(b, s2, sizeof s2, p2, sizeof p2) &&
                                    strcmp(s2, s_scan[found].ssid) == 0) {
                                    s_scan[found].saved = (uint8_t)(b + 1);
                                    break;
                                }
                            }
                            found++;
                        }
                        free(ap);
                    }
                    esp_wifi_scan_stop();
                }
                esp_wifi_stop();
            }
            esp_wifi_deinit();
        }
        badge_wifi_give();
    }
    ESP_LOGI("wifi", "scan finished — %d networks", found);
    s_scan_n = found;
    s_scan_busy = false;
    vTaskDelete(NULL);
}

void port_wifi_scan_start(void)
{
    if (s_scan_busy) return;
    s_scan_busy = true;
    s_scan_n = -1;
    /* 🚨 Check that the task was actually created. Internal RAM runs thin
     * (it is down to 22 KB while the clock sync holds WiFi), and xTaskCreate
     * takes the TCB and the 4 KB stack from it. When it fails, s_scan_n stays
     * -1 ("still scanning") and s_scan_busy latches true — the screen never
     * leaves "looking around..." and no later scan runs either, until a
     * reboot. Answer "nothing found" and release the latch instead. */
    if (xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE("wifi", "could not create the scan task - out of internal RAM");
        s_scan_n = 0;
        s_scan_busy = false;
    }
}

int port_wifi_scan_result(wifi_found_t *out, int max)
{
    int n = s_scan_n;
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, s_scan, n * sizeof(wifi_found_t));
    return n;
}

/* ── trying to join ────────────────────────────────────────── */
static char          s_try_ssid[33], s_try_pass[65];
static volatile int  s_try_state = WIFI_TRY_FAIL;
static volatile bool s_try_busy;

static void try_task(void *arg)
{
    (void)arg;
    int ok = WIFI_TRY_FAIL;
    if (badge_wifi_take(8000)) {
        esp_netif_t *nif = badge_wifi_netif_once();
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&ic) == ESP_OK) {
            wifi_config_t wc = { 0 };
            snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s_try_ssid);
            snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", s_try_pass);
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            if (esp_wifi_start() == ESP_OK) {
                esp_wifi_connect();
                /* 🚨 "Connected" means an address was obtained. A link
                 * without DHCP can do nothing — that is where the line is. */
                esp_netif_ip_info_t ip = { 0 };
                for (int i = 0; i < 30; i++) {          /* up to 15 seconds */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
                        ok = WIFI_TRY_OK;
                        break;
                    }
                }
                esp_wifi_disconnect();
                esp_wifi_stop();
            }
            esp_wifi_deinit();
        }
        badge_wifi_give();
    }
    if (ok == WIFI_TRY_OK) {
        nvs_handle_t nh;
        if (nvs_open(WIFI_NS, NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "wifi_last", s_try_ssid);
            nvs_commit(nh);
            nvs_close(nh);
        }
    }
    ESP_LOGI("wifi", "join %s (SSID %s)",
             ok == WIFI_TRY_OK ? "succeeded" : "failed", s_try_ssid);
    memset(s_try_pass, 0, sizeof s_try_pass);   /* no reason to hold it any longer */
    s_try_state = ok;
    s_try_busy = false;
    vTaskDelete(NULL);
}

void port_wifi_try(const char *ssid, const char *pass)
{
    if (s_try_busy) return;
    snprintf(s_try_ssid, sizeof s_try_ssid, "%s", ssid ? ssid : "");
    if (pass) {
        snprintf(s_try_pass, sizeof s_try_pass, "%s", pass);
    } else {
        /* Use what is stored — no reason to retype a network already saved. */
        int b = port_wifi_slot_find(s_try_ssid);
        char s2[33];
        s_try_pass[0] = '\0';
        if (b >= 0) {
            char k[24];
            slot_key(k, sizeof k, "wifi_pass", b);
            creds_get(k, s_try_pass, sizeof s_try_pass);
            (void)s2;
        }
    }
    s_try_busy = true;
    s_try_state = WIFI_TRY_BUSY;
    /* 🚨 Same place as the scan: without this the state stays WIFI_TRY_BUSY
     * and "connecting..." never ends. */
    if (xTaskCreate(try_task, "wifitry", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE("wifi", "could not create the join task - out of internal RAM");
        s_try_state = WIFI_TRY_FAIL;
        s_try_busy = false;
    }
}

int port_wifi_try_state(void) { return s_try_state; }

void port_wifi_last_ok(char *ssid, size_t ss)
{
    creds_get("wifi_last", ssid, ss);
}

/* ── writing and clearing slots ────────────────────────────── */
static void slot_write(int slot, const char *key, const char *val)
{
    char k[24];
    slot_key(k, sizeof k, key, slot);
    nvs_handle_t nh;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &nh) != ESP_OK) return;
    if (val && val[0]) nvs_set_str(nh, k, val);
    else               nvs_erase_key(nh, k);
    nvs_commit(nh);
    nvs_close(nh);
}

void port_wifi_slot_set(int slot, const char *ssid, const char *pass)
{
    if (slot < 0 || slot >= WIFI_SLOTS) return;
    slot_write(slot, "wifi_ssid", ssid);
    slot_write(slot, "wifi_pass", pass);
    ESP_LOGI("wifi", "stored in slot %d (SSID %s)", slot + 1, ssid ? ssid : "");
}

void port_wifi_slot_clear(int slot)
{
    if (slot < 0 || slot >= WIFI_SLOTS) return;
    slot_write(slot, "wifi_ssid", NULL);
    slot_write(slot, "wifi_pass", NULL);
    ESP_LOGI("wifi", "cleared slot %d", slot + 1);
}

int port_wifi_slot_find(const char *ssid)
{
    if (!ssid || !ssid[0]) return -1;
    char s2[33], p2[65];
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (creds_wifi_slot(i, s2, sizeof s2, p2, sizeof p2) && strcmp(s2, ssid) == 0) {
            memset(p2, 0, sizeof p2);
            return i;
        }
    return -1;
}

int port_wifi_slot_free(void)
{
    char s2[33], p2[65];
    for (int i = 0; i < WIFI_SLOTS; i++)
        if (!creds_wifi_slot(i, s2, sizeof s2, p2, sizeof p2)) return i;
    /* 🚨 When full, push out the last slot. Slot 1 is where the secrets.h
     * seed lands, so it is kept if possible — without it a new badge has
     * nothing to join at all. */
    return WIFI_SLOTS - 1;
}

bool port_wifi_slot_get(int slot, char *ssid, size_t ss)
{
    char p2[65];
    if (slot < 0 || slot >= WIFI_SLOTS) { if (ss) ssid[0] = '\0'; return false; }
    /* 🚨 The password is taken and discarded. It is never returned to the UI. */
    bool ok = creds_wifi_slot(slot, ssid, ss, p2, sizeof p2);
    memset(p2, 0, sizeof p2);
    return ok;
}

bool badge_wifi_pick(char *ssid, size_t ss, char *pass, size_t ps)
{
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW("wifi", "scan failed — falling back to slot 1");
        return badge_creds_wifi_live(ssid, ss, pass, ps);
    }
    uint16_t got = 0;
    esp_wifi_scan_get_ap_num(&got);
    /* 🚨 Internal RAM is thin (twenty-odd KB with BLE up). An entry is about
     * 80 bytes, so sixteen is 1.3 KB — that is the cut-off. They arrive
     * strongest first, so little is lost. */
    uint16_t want = got > 16 ? 16 : got;
    wifi_ap_record_t *ap = want ? calloc(want, sizeof *ap) : NULL;
    if (ap) esp_wifi_scan_get_ap_records(&want, ap);
    else    want = 0;

    int best = -1, best_rssi = -128;
    char bs[33] = "", bp[65] = "";
    for (int i = 0; i < WIFI_SLOTS; i++) {
        char s2[33], p2[65];
        if (!creds_wifi_slot(i, s2, sizeof s2, p2, sizeof p2)) continue;
        for (int j = 0; j < want; j++) {
            if (strcmp((const char *)ap[j].ssid, s2) != 0) continue;
            if (ap[j].rssi > best_rssi) {
                best_rssi = ap[j].rssi;
                best = i;
                snprintf(bs, sizeof bs, "%s", s2);
                snprintf(bp, sizeof bp, "%s", p2);
            }
            break;
        }
    }
    free(ap);
    esp_wifi_scan_stop();

    if (best >= 0) {
        ESP_LOGI("wifi", "scan found %d — using %s (%d dBm, slot %d)",
                 (int)got, bs, best_rssi, best + 1);
        snprintf(ssid, ss, "%s", bs);
        snprintf(pass, ps, "%s", bp);
        return true;
    }
    /* 🚨 Finding nothing is not a reason to give up — it may be hidden, or
     * simply missed during that scan. Slot 1 gets one attempt. */
    ESP_LOGW("wifi", "none of the %d scanned are known — trying slot 1", (int)got);
    return badge_creds_wifi_live(ssid, ss, pass, ps);
}

bool badge_creds_wifi_live(char *ssid, size_t ss, char *pass, size_t ps)
{
    if (badge_creds_wifi(ssid, ss, pass, ps)) return true;
    wifi_config_t wc = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK || !wc.sta.ssid[0]) {
        ESP_LOGW("wifi", "no credentials on the badge and nothing to recover");
        return false;
    }
    snprintf(ssid, ss, "%s", (const char *)wc.sta.ssid);
    snprintf(pass, ps, "%s", (const char *)wc.sta.password);
    creds_seed("wifi_ssid", ssid);
    creds_seed("wifi_pass", pass);
    ESP_LOGI("wifi", "recovered the credentials the stack had stored (SSID %s)", ssid);
    return true;
}

static volatile net_state_t s_net = NET_IDLE;

/* 🚨 This used to branch on `#if !defined(BADGE_WIFI_SSID)`. Credentials now
 * live on the badge rather than in the build, so whether there are any is a
 * runtime question. */
/* 🚨 esp_netif_create_default_wifi_sta() asserts if called twice (caught by
 * the self-test on the second upload). Both the clock and the recording
 * upload used WiFi, so it is created in exactly one place and shared. The
 * netif is never destroyed — destroying and recreating is the same trap. */
/* 🚨 Only one user of WiFi at a time.
 * The clock sync and the upload each called esp_wifi_init, and overlapping
 * them brings the same hardware up twice. A slow clock sync at boot colliding
 * with the 30-second upload poll is a situation that really occurs.
 * Whoever takes it first uses it and the other backs off — both will be back. */
static SemaphoreHandle_t s_wifi_gate;

bool badge_wifi_take(uint32_t wait_ms)
{
    if (!s_wifi_gate) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
        if (!s_wifi_gate) s_wifi_gate = xSemaphoreCreateMutex();
        portEXIT_CRITICAL(&mux);
    }
    if (!s_wifi_gate) return true;          /* if it could not be created, do not block */
    return xSemaphoreTake(s_wifi_gate, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void badge_wifi_give(void)
{
    if (s_wifi_gate) xSemaphoreGive(s_wifi_gate);
}

esp_netif_t *badge_wifi_netif_once(void)
{
    static esp_netif_t *nif;
    if (!nif) {
        esp_netif_init();
        esp_event_loop_create_default();      /* fails quietly if one already exists */
        nif = esp_netif_create_default_wifi_sta();
    }
    return nif;
}

static void sync_task(void *arg)
{
    (void)arg;
    if (!badge_wifi_take(1000)) {           /* something else has it */
        ESP_LOGI("net", "WiFi is in use elsewhere — clock sync will wait");
        s_net = NET_IDLE;
        vTaskDelete(NULL);
        return;
    }
    s_net = NET_CONNECTING;
    char ss[33] = "", pw[65] = "";

    badge_wifi_netif_once();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    /* 🚨 Trying to bring it down when it never came up leaves two lines of
     * 0x3001 and hides the real reason. Log why and back out here. */
    esp_err_t we = esp_wifi_init(&ic);
    if (we != ESP_OK) {
        ESP_LOGE("net", "could not bring WiFi up: %s — giving up on the clock", esp_err_to_name(we));
        s_net = NET_FAIL;
        badge_wifi_give();
        vTaskDelete(NULL);
        return;
    }
    /* 🚨 Scanning needs it up first, so the order is: start, choose, join. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (!badge_wifi_pick(ss, sizeof ss, pw, sizeof pw)) {
        ESP_LOGW("net", "no WiFi credentials — skipping the clock");
        s_net = NET_NOCONF;
        esp_wifi_stop();
        esp_wifi_deinit();
        badge_wifi_give();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI("net", "setting the clock (SSID %s)", ss);
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ss);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pw);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI("net", "joining WiFi");

    /* 🚨 Success used to be judged as "the clock reads later than 2023". But
     * if a (wrong) time is already set at boot, that is **true on the first
     * check** — it declared success before NTP had answered and switched SNTP
     * off. Only the very first run (with the clock at 1970) actually worked;
     * every one after that spun for nothing. That is why the badge stayed
     * eight or nine minutes fast no matter how many times it was reflashed.
     * 🚨 Never use a condition that is only true when the thing is missing as
     * a success test — once it succeeds it answers "succeeded" while doing
     * nothing. Use the callback SNTP invokes when it has actually set a time. */
    s_sntp_done = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_got_time);
    esp_sntp_init();

    time_t before = time(NULL);
    /* Wait up to twenty seconds, then give up and switch the radio off. */
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_sntp_done) break;
    }
    s_net = s_sntp_done ? NET_SYNCED : NET_FAIL;
    if (s_sntp_done) {
        /* Record how far off it was — the error from mis-counting sleep shows
         * up here directly, and it is what sets the resync interval. */
        long off = (long)(time(NULL) - before);
        ESP_LOGI("net", "clock set (%+ld s off)", off);
    }
    if (s_net == NET_SYNCED) {
        rtc_write_now();          /* so it survives losing power */
        time_synced_now();        /* the resync clock restarts from here */
    }

    esp_sntp_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI("net", "clock sync %s", s_net == NET_SYNCED ? "succeeded" : "failed");
    badge_wifi_give();
    vTaskDelete(NULL);
}

void port_time_sync_start(void)
{
    /* With no credentials on the badge there is nowhere to go */
    if (!badge_creds_wifi_any()) { s_net = NET_NOCONF; return; }
    if (s_net == NET_CONNECTING) return;
    /* 🚨 The same check here. A missing clock sync does not hold the screen,
     * but it is better named in the log than silently absent. */
    if (xTaskCreate(sync_task, "timesync", 4096, NULL, 4, NULL) != pdPASS)
        ESP_LOGE("net", "could not create the time sync task - out of internal RAM");
}

/* The 1.75C has no RTC chip — losing power loses the time. (The plain 1.75
 * does have one, a PCF85063, which is why the driver below exists at all; on
 * this board it probes and finds nothing.) So it goes and sets the clock once
 * after boot.
 * 🚨 It used to sync only "when the clock was not set", which meant that once
 * set it **never synced again**. With no RTC the time runs off the chip's own
 * oscillator and gains minutes a day (reported as three or four minutes ahead
 * of a phone). It resyncs periodically even when already set. */
/* 🚨 Six hours was not enough. This chip measures elapsed sleep with its
 * **internal RC oscillator** (CONFIG_RTC_CLK_SRC_INT_RC), which is off by
 * about 1%. With the display off the badge is in light sleep almost
 * continuously (PM_ENABLE + TICKLESS_IDLE), so that error accumulates
 * directly — eight minutes gained in half a day (8 min / 12 h = 1.1%, exactly
 * the RC error).
 * This is not crystal drift, it is **mis-counting time asleep**, and it does
 * not go away without changing the oscillator. Syncing often keeps the error
 * boxed in: at one hour it stays under forty seconds.
 * One sync costs about eight seconds of WiFi (roughly 0.3 mAh), which is lost
 * in the standby draw.
 * ⏭ The real fix is a 32.768 kHz crystal (20 ppm). First find out whether the
 *    board has one — without it IDF logs a note and falls back to RC. */
#define RESYNC_SEC  (1 * 3600)
static time_t s_last_sync;

static void time_synced_now(void) { s_last_sync = time(NULL); }

void port_time_autosync(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) { port_time_sync_start(); return; }
    /* Right after boot s_last_sync is 0, which counts as "stale" and syncs once */
    if (s_last_sync == 0 || now - s_last_sync > RESYNC_SEC) port_time_sync_start();
}
net_state_t port_time_sync_state(void) { return s_net; }

const char *port_bt_status(void)
{
    return "off";        /* with BLE HID attached this returns the host's name */
}

/* ── how many fingers ─────────────────────────────────────────
 * The BSP hides the touch handle, but the LVGL adapter stores it in the
 * indev's driver_data. Only the front of that struct (magic plus handle) is
 * borrowed — the magic is checked, so if the adapter ever changes this
 * returns zero rather than being quietly wrong.
 *
 * read_data is not called: the adapter has already read this cycle, so only
 * the cached coordinates are taken and the I2C bus is not hit twice. */
#include "esp_lcd_touch.h"

#define ADAPTER_TOUCH_CTX_MAGIC  UINT32_C(0x54435458)

typedef struct {
    uint32_t magic;
    esp_lcd_touch_handle_t handle;
} adapter_touch_head_t;

int port_touch_count(void)
{
    static esp_lcd_touch_handle_t tp;
    static bool looked;

    if (!looked) {
        looked = true;
        lv_indev_t *indev = badge_display_indev();
        if (indev) {
            adapter_touch_head_t *h = lv_indev_get_driver_data(indev);
            if (h && h->magic == ADAPTER_TOUCH_CTX_MAGIC) tp = h->handle;
        }
        if (!tp) ESP_LOGW("touch", "touch handle not found — two-finger gestures are off");
    }
    if (!tp) return 1;

    uint16_t x[2], y[2];
    uint8_t n = 0;
    esp_lcd_touch_get_coordinates(tp, x, y, NULL, &n, 2);
    return n;
}

/* Display off means the panel is actually switched off.
 *
 * It used to only set the brightness to zero (which is all the BSP's
 * backlight_off does). The pixels go dark but the driver, the gate scan and
 * the boost converter all keep running — measured, "off" still drew 44% of
 * "on" (76 mV/h against 171 mV/h).
 * Now 0x28 (Display Off) stops the scan. How much that saved shows up
 * immediately if measured the same way.
 *
 * Restoring the brightness is still done here: the BSP's backlight_on always
 * goes to 100%, which threw away the user's setting on every wake. */
/* ── putting the touch chip to sleep ──────────────────────────
 * The CST9217 keeps scanning even with the display off. Its power rail is
 * shared with the LCD and VCC3V3 and cannot be cut, but it can be told to
 * sleep.
 *
 * Sources (all local): SensorLib's CST9xxConstants.h defines
 * CST9217_CHIP_ID=0x9217 alongside CST92XX_REG_SLEEP_MODE=0xD105, and
 * sleep() in TouchDrvCST92xx/CST226 sends the two bytes {0xD1,0x05}.
 * Hynitron's porting manual lists "0xD105 Deep sleep" on p.20.
 * The registers our driver already uses (0xD000/0xD101/0xD1FC/0xD1F8) map
 * one-to-one onto the same family table — this is a different family from the
 * CST816-style 0xA5 command.
 *
 * Waking is done by toggling RST (GPIO2). I2C may not respond while asleep,
 * so waking by command is not the way. RST is independent of the LCD's
 * (GPIO1), so touch can be revived without touching the display. */
#define TP_ADDR      0x5A
#define TP_RST_GPIO  2

static i2c_master_dev_handle_t s_tp;
static bool                    s_tp_asleep;

static void tp_open(void)
{
    if (s_tp) return;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TP_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_tp) != ESP_OK) s_tp = NULL;
}

static void tp_sleep(bool on)
{
    if (on == s_tp_asleep) return;
    tp_open();
    if (!s_tp) return;

    if (on) {
        uint8_t cmd[2] = { 0xD1, 0x05 };           /* Deep sleep */
        if (i2c_master_transmit(s_tp, cmd, 2, 100) == ESP_OK) {
            s_tp_asleep = true;
            ESP_LOGI("tp", "touch chip asleep (0xD1 0x05)");
        }
    } else {
        gpio_set_direction(TP_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(TP_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TP_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(50));             /* the datasheet's recommended settling time */
        s_tp_asleep = false;
        ESP_LOGI("tp", "touch chip awake (RST toggled)");
    }
}

void port_display_power(bool on)
{
    badge_display_on(on);
    /* Touch does not wake the display by design (pocket protection), so there
     * is no reason for the touch chip to scan while it is off. */
    tp_sleep(!on);
}

/* ── the PWR button ───────────────────────────────────────────
 * Unlike BOOT this is not a GPIO. The AXP2101 catches the press and flags it
 * in an interrupt status register, which we read over I2C — so it is very
 * slightly late. Held down, the chip cuts power in hardware and there is
 * nothing we can do about it. */
#include "driver/i2c_master.h"

#define AXP_ADDR        0x34
#define AXP_REG_INTEN2  0x41
#define AXP_REG_INTSTS2 0x49
#define AXP_PKEY_LONG   0x04     /* combined IRQ bit 10 */
#define AXP_PKEY_SHORT  0x08     /* combined IRQ bit 11 */

static i2c_master_dev_handle_t s_axp;

static bool axp_rd(uint8_t reg, uint8_t *v)
{
    if (!s_axp) axp_init();          /* this can be called before the button task */
    return s_axp && i2c_master_transmit_receive(s_axp, &reg, 1, v, 1, 100) == ESP_OK;
}

static bool axp_wr(uint8_t reg, uint8_t v)
{
    uint8_t buf[2] = { reg, v };
    return s_axp && i2c_master_transmit(s_axp, buf, 2, 100) == ESP_OK;
}

static void axp_init(void)
{
    if (s_axp) return;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        s_axp = NULL;
        ESP_LOGW("axp", "could not reach the PMU — the PWR button is off");
        return;
    }
    /* ── switching off unused power rails ───────────────────
     * Checking the schematic, this board actually uses two:
     *   DCDC1 -> VCC3V3 (ESP32-S3, LCD/touch FPC, codec digital, IMU, amp)
     *   ALDO1 -> A3V3   (ES8311/ES7210 analogue, mic bias)
     * The other twelve go nowhere. DCDC2-5 do not even have inductors.
     *
     * And these registers had never been touched — the AXP was running on its
     * boot defaults (EFUSE). Some datasheet default layouts bring DCDC2, 3, 4
     * and ALDO3 up enabled, and an enabled buck with no inductor never sees
     * its feedback rise, so it switches at maximum duty or hiccups forever.
     *
     * Log the current state first, then clean up the way Waveshare's own
     * example does. */
    uint8_t d0 = 0, l0 = 0, l1 = 0, v1 = 0, va = 0;
    axp_rd(0x80, &d0); axp_rd(0x90, &l0); axp_rd(0x91, &l1);
    axp_rd(0x82, &v1); axp_rd(0x92, &va);
    ESP_LOGI("axp", "before  DCDC=0x%02X LDO=0x%02X/0x%02X  DCDC1=%dmV ALDO1=%dmV",
             d0, l0, l1, 500 + v1 * 10, 500 + va * 100);

    axp_wr(0x80, (uint8_t)((d0 & ~0x1E) | 0x01));   /* leave only DCDC1 */
    axp_wr(0x90, 0x01);                              /* leave only ALDO1 */
    axp_wr(0x91, (uint8_t)(l1 & ~0x01));             /* DLDO2 off */

    axp_rd(0x80, &d0); axp_rd(0x90, &l0); axp_rd(0x91, &l1);
    ESP_LOGI("axp", "after   DCDC=0x%02X LDO=0x%02X/0x%02X", d0, l0, l1);

    /* ── checking the charging configuration ────────────────
     * We do not control charging at all and leave it to the chip's defaults.
     * Whether those defaults suit this battery is worth knowing, so they are
     * logged once at boot. */
    {
        uint8_t ipre = 0, icc = 0, iterm = 0, cv = 0, voff = 0, vsys = 0, chg = 0;
        axp_rd(0x61, &ipre); axp_rd(0x62, &icc); axp_rd(0x63, &iterm);
        axp_rd(0x64, &cv);   axp_rd(0x24, &voff); axp_rd(0x14, &vsys);
        axp_rd(0x01, &chg);
        /* Charge current: 0-8 are 25 mA steps, 9 onward is 300/400/500... */
        int cc = (icc & 0x1F);
        int cc_ma = (cc <= 8) ? cc * 25 : 300 + (cc - 9) * 100;
        static const char *CV[] = { "-", "4.00V", "4.10V", "4.20V", "4.35V", "4.40V", "?", "?" };
        static const char *ST[] = { "idle", "pre-charge", "CC", "CV", "done", "stopped", "?", "?" };
        ESP_LOGI("axp", "charge  current %dmA, termination %dmA, full %s, pre-charge %dmA",
                 cc_ma, (iterm & 0x0F) * 25, CV[cv & 0x07], (ipre & 0x0F) * 25);
        ESP_LOGI("axp", "protect cutoff %.1fV, system min %.2fV, now %s",
                 2.6 + (voff & 0x07) * 0.1, 4.1 + (vsys & 0x07) * 0.1,
                 ST[(chg >> 5) & 0x07]);
    }

    /* ── correcting the charging defaults ───────────────────
     * What they actually were:
     *   termination current 125 mA — 62% of the charge current (200 mA). That
     *     calls it full at around 60%. It should be 5-10% of the charge
     *     current to actually reach full -> 25 mA
     *   pre-charge 125 mA — a flat lithium cell has to be woken gently -> 25 mA
     *   cutoff 2.6 V — below 3.0 V lithium takes damage it does not come back
     *     from. This was the worst of the three -> 3.0 V
     * The charge current (200 mA) is left alone: raising it without knowing
     * the cell's capacity is unwise, and 200 mA is not hard on a cell this size. */
    {
        uint8_t t = 0, ip = 0, vo = 0;
        axp_rd(0x63, &t);  axp_wr(0x63, (uint8_t)((t & 0xF0) | 0x01));   /* terminate at 25mA */
        axp_rd(0x61, &ip); axp_wr(0x61, (uint8_t)((ip & 0xF0) | 0x01));  /* pre-charge 25mA */
        axp_rd(0x24, &vo); axp_wr(0x24, (uint8_t)((vo & 0xF8) | 0x04));  /* cutoff 3.0V */
        axp_rd(0x63, &t);  axp_rd(0x61, &ip); axp_rd(0x24, &vo);
        ESP_LOGI("axp", "corrected  termination %dmA, pre-charge %dmA, cutoff %.1fV",
                 (t & 0x0F) * 25, (ip & 0x0F) * 25, 2.6 + (vo & 0x07) * 0.1);
    }

    uint8_t v = 0;
    axp_rd(AXP_REG_INTEN2, &v);
    axp_wr(AXP_REG_INTEN2, v | AXP_PKEY_SHORT | AXP_PKEY_LONG);
    axp_wr(AXP_REG_INTSTS2, AXP_PKEY_SHORT | AXP_PKEY_LONG);   /* clear anything stale */
}

int port_pwr_key(void)
{
    static int64_t last_us;

    uint8_t st = 0;
    if (!axp_rd(AXP_REG_INTSTS2, &st)) return 0;
    uint8_t hit = st & (AXP_PKEY_SHORT | AXP_PKEY_LONG);
    if (!hit) return 0;
    axp_wr(AXP_REG_INTSTS2, st);             /* clear exactly the bits that were read */

    /* One press counted twice switches it off the instant it comes on. One per 0.4 s. */
    int64_t now = esp_timer_get_time();
    if (now - last_us < 400000) return 0;
    last_us = now;

    return (hit & AXP_PKEY_SHORT) ? 1 : 2;
}

/* ── PCF85063 RTC ─────────────────────────────────────────────
 * The ESP's internal clock is lost with power, so an RTC running from its own
 * cell would be read at boot and written whenever the clock is set.
 * 🚨 The 1.75C does not have one. I2C answers at 18 34 40 5A 6B and nothing at
 * 0x51, so every one of these calls fails and says so in the log — that is
 * expected, not a fault. It is kept because the plain 1.75 carries a PCF85063
 * at this address, and because the cost of probing once at boot is nothing.
 * Without an RTC, every power-on is 1970 until the clock syncs. */
#define RTC_ADDR      0x51
#define RTC_REG_SEC   0x04       /* sec, min, hour, day, weekday, month, year — 7 bytes BCD */

static i2c_master_dev_handle_t s_rtc;

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static bool rtc_open(void)
{
    if (s_rtc) return true;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_ADDR,
        .scl_speed_hz    = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_rtc) == ESP_OK;
}

static void rtc_write_now(void)
{
    if (!rtc_open()) return;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);                 /* the chip is written in UTC */
    uint8_t b[8] = {
        RTC_REG_SEC,
        dec2bcd(tm.tm_sec) & 0x7F,       /* bit7 flags a stopped oscillator; clear it */
        dec2bcd(tm.tm_min),
        dec2bcd(tm.tm_hour),
        dec2bcd(tm.tm_mday),
        dec2bcd(tm.tm_wday),
        dec2bcd(tm.tm_mon + 1),
        dec2bcd(tm.tm_year % 100),
    };
    /* 🚨 rtc_open() only registers a device on the bus — it does not go and
     * look, so it succeeds on a board with no RTC and the write then NACKs.
     * The return used to be thrown away, and the log said "written to the RTC"
     * on a 1.75C, which has none. It is said once and then left alone; on this
     * board it is the expected outcome, not an hourly complaint. */
    if (i2c_master_transmit(s_rtc, b, sizeof(b), 200) == ESP_OK) {
        ESP_LOGI("rtc", "written to the RTC");
        return;
    }
    static bool said;
    if (!said) {
        said = true;
        ESP_LOGW("rtc", "no RTC at 0x%02X to write to — the time will not "
                        "survive a power cut (normal on the 1.75C)", RTC_ADDR);
    }
}

/* The time zone is held as minutes offset from UTC and turned into a POSIX
 * string. POSIX has the opposite sign — Seoul (UTC+9) is "<+09>-9". */
static int s_tz_min = 9 * 60;

void port_set_tz_offset(int minutes)
{
    char tz[32];
    int m = -minutes;                       /* flip the sign */
    int h = m / 60, r = abs(m % 60);
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", h, r);
    setenv("TZ", tz, 1);
    tzset();
    s_tz_min = minutes;

    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_i32(nh, "tz", minutes);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

int port_get_tz_offset(void) { return s_tz_min; }

void port_rtc_restore(void)
{
    int32_t tz = 9 * 60;
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_i32(nh, "tz", &tz);
        nvs_close(nh);
    }
    port_set_tz_offset(tz);

    /* Probe once for what is on which address — with a wrong address the
     * failure is silent and leaves no log at all. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus) {
        char found[96] = {0};
        int n = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 50) == ESP_OK && n < 80) {
                n += snprintf(found + n, sizeof(found) - n, "%02X ", a);
            }
        }
        ESP_LOGI("i2c", "devices: %s", found);
    }

    if (!rtc_open()) { ESP_LOGW("rtc", "could not register the I2C device"); return; }
    uint8_t reg = RTC_REG_SEC, d[7] = {0};
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, d, sizeof(d), 200) != ESP_OK) {
        ESP_LOGW("rtc", "no RTC responding (address 0x%02X)", RTC_ADDR);
        return;
    }

    if (d[0] & 0x80) {                   /* the oscillator has stopped — do not trust it */
        ESP_LOGW("rtc", "the RTC is empty — the clock needs setting");
        return;
    }
    struct tm tm = {
        .tm_sec  = bcd2dec(d[0] & 0x7F),
        .tm_min  = bcd2dec(d[1] & 0x7F),
        .tm_hour = bcd2dec(d[2] & 0x3F),
        .tm_mday = bcd2dec(d[3] & 0x3F),
        .tm_mon  = bcd2dec(d[5] & 0x1F) - 1,
        .tm_year = bcd2dec(d[6]) + 100,
    };
    if (tm.tm_year < 120) return;        /* anything before 2020 is garbage */

    /* newlib has no timegm. Switch TZ to UTC briefly and use mktime. */
    setenv("TZ", "UTC0", 1); tzset();
    time_t t = mktime(&tm);
    setenv("TZ", "KST-9", 1); tzset();
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    ESP_LOGI("rtc", "clock restored from the RTC: %04d-%02d-%02d %02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* ── tilt ─────────────────────────────────────────────────────
 * Gravity's direction, read from the QMI8658. The angle of the component
 * lying in the screen plane (X and Y) is "how far the badge is rotated". Flat
 * on a table that component vanishes and the angle becomes noise — which is
 * reported rather than returned. */
#include "qmi8658.h"

static qmi8658_dev_t s_imu;
static bool          s_imu_ok;
static bool          s_imu_tried;
/* 🚨 Once the accelerometer is switched on it runs at 125 Hz forever. One
 * round of the marble game or a moment of auto-rotate left it on for good,
 * display off included.
 * Five seconds since the last read puts it to sleep; the next read wakes it.
 * Waking takes a few milliseconds, so games do not notice. */
static bool     s_imu_awake;
static int64_t  s_imu_last_us;
/* 🚨 For the first few milliseconds after waking there is no valid reading
 * (at 125 Hz a sample is 8 ms). Read in that window and you get the previous
 * value or zero — and the games, the water and the air mouse all take that
 * first value as "this attitude is level". The reference lands somewhere
 * wrong and the paddle sits at one end and will not move, which is why the
 * first round of bricks never worked until you had died once. Right after
 * waking, this answers "not yet known". */
static int64_t  s_imu_wake_us;
#define IMU_SETTLE_US  30000

static void imu_touch(void)      /* mark it as just used */
{
    s_imu_last_us = esp_timer_get_time();
    if (s_imu_ok && !s_imu_awake) {
        qmi8658_enable_accel(&s_imu, true);
        s_imu_awake = true;
        s_imu_wake_us = s_imu_last_us;
    }
}

/* Too soon after waking to be trusted */
static bool imu_settled(void)
{
    return s_imu_wake_us == 0 ||
           esp_timer_get_time() - s_imu_wake_us >= IMU_SETTLE_US;
}

void port_imu_idle_check(void)   /* the launcher calls this periodically */
{
    if (!s_imu_ok || !s_imu_awake) return;
    if (esp_timer_get_time() - s_imu_last_us < 5000000LL) return;
    qmi8658_enable_accel(&s_imu, false);
    s_imu_awake = false;
    ESP_LOGI("imu", "unused for 5 s — accelerometer asleep");
}

static float s_ax, s_ay, s_az;

/* "Held upright" means the in-plane component is clearly larger than the
 * screen normal (Z). Flat on a desk Z dominates and the angle is meaningless. */
bool port_imu_upright(void)
{
    float plane = sqrtf(s_ax * s_ax + s_ay * s_ay);
    return plane > 700.0f && plane > fabsf(s_az);
}

/* All three axes as they come, in mg (1 g is about 1000).
 * 🚨 With only x and y you know whether it is tilted and nothing else. A
 * shake changes the magnitude, and that lives in all three — anything that
 * has to respond to shaking, like the water, needs this. */
bool port_imu_accel3(float *x, float *y, float *z)
{
    float d;
    port_imu_angle(&d);              /* forces a fresh read */
    if (x) *x = s_ax;
    if (y) *y = s_ay;
    if (z) *z = s_az;
    return s_imu_ok && imu_settled();
}

bool port_imu_accel(float *x, float *y)
{
    float d;
    port_imu_angle(&d);
    *x = s_ax; *y = s_ay;
    return s_imu_ok && imu_settled();
}

/* 🚨 Initialisation was pulled out to here. It used to live inside
 * port_imu_angle, which left anything using only the gyro (the air mouse)
 * with no way to bring the chip up. */
static bool imu_ready(void)
{
    if (!s_imu_tried) {
        s_imu_tried = true;
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (bus && qmi8658_init(&s_imu, bus, 0x6B) == ESP_OK) {
            qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
            qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_125HZ);
            qmi8658_enable_accel(&s_imu, true);
            s_imu_awake = true;
            s_imu_ok = true;
            /* 🚨 Just after power-on is as untrustworthy as just after
             * waking. Without catching it here, the first read after boot is
             * x=y=z=4000 — an impossible value — and still reports as
             * trustworthy (caught by the bench). */
            s_imu_wake_us = esp_timer_get_time();
            ESP_LOGI("imu", "QMI8658 found");
        } else {
            ESP_LOGW("imu", "QMI8658 init failed — tilt compensation is off");
        }
    }
    return s_imu_ok;
}

/* ── gyro ─────────────────────────────────────────────────────
 * 🔋 Once on it stays on until whoever switched it on switches it off. It has
 * no "sleep when unused" like the accelerometer because holding the air mouse
 * still and aiming for several seconds is normal, and treating that as idle
 * would be wrong. */
static bool    s_gyro_on;
static int64_t s_gyro_wake_us;
/* 🚨 40 ms was not enough. A gyro emits unsettled values for a while after
 * being switched on, and taking those as the zero leaves the cursor drifting
 * by that much forever. */
#define GYRO_SETTLE_US 120000

void port_imu_gyro_enable(bool on)
{
    if (!imu_ready()) return;
    if (on == s_gyro_on) return;
    if (on) {
        /* 512 dps takes a hard flick of the wrist without clipping (a human
         * wrist is roughly 500). The ODR only has to be comfortably above the
         * report rate (66 per second). */
        qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_512DPS);
        qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_250HZ);
        /* 🚨 The unit is pinned here. Initialisation does set degrees, but if
         * this one value ever flipped to radians the cursor would be 57 times
         * slower and the cause would be hard to find — so the code says out
         * loud that it reads degrees. */
        s_imu.gyro_unit_rads = false;
        qmi8658_enable_gyro(&s_imu, true);
        s_gyro_wake_us = esp_timer_get_time();
    } else {
        qmi8658_enable_gyro(&s_imu, false);
    }
    s_gyro_on = on;
    ESP_LOGI("imu", "gyro %s", on ? "on" : "off");
}

bool port_imu_gyro(float *x, float *y, float *z)
{
    /* 🚨 Not trustworthy right after switch-on — the same trap as the
     * accelerometer returning an impossible first value and anchoring a
     * reference to it. */
    if (!s_gyro_on) return false;
    if (esp_timer_get_time() - s_gyro_wake_us < GYRO_SETTLE_US) return false;

    /* 🚨 The header declares qmi8658_read_gyro_dps() but **there is no
     * implementation** (the vendor driver declared it and stopped — you find
     * out at link time). What exists is qmi8658_read_gyro(), which picks its
     * unit from gyro_unit_rads. */
    float gx = 0, gy = 0, gz = 0;
    if (qmi8658_read_gyro(&s_imu, &gx, &gy, &gz) != ESP_OK) return false;
    if (x) *x = gx;
    if (y) *y = gy;
    if (z) *z = gz;
    return true;
}

bool port_imu_angle(float *deg)
{
    if (!imu_ready()) return false;
    imu_touch();

    float x = 0, y = 0, z = 0;
    if (qmi8658_read_accel(&s_imu, &x, &y, &z) != ESP_OK) return false;

    /* The driver reports mg, not g (1 g is about 1000) — confirmed against
     * board logs. Below 0.35 g in the screen plane it is lying flat and the
     * angle is noise. */
    s_ax = x; s_ay = y; s_az = z;

    float mag = sqrtf(x * x + y * y);
    if (mag < 350.0f) return false;

    *deg = atan2f(x, y) * 57.29578f;

    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last > 2000000) {
        last = now;
        ESP_LOGI("imu", "accel x=%.2f y=%.2f z=%.2f -> %.0f deg", x, y, z, *deg);
    }
    return true;
}

/* ── battery ──────────────────────────────────────────────────
 * The AXP2101 works out the charge level and puts it in a register, so there
 * is no need to guess from voltage. In status 2 (0x01), bit 3 is
 * battery-present and bits 5-6 are charging. */
#define AXP_REG_STATUS1   0x00      /* bit3 = battery present (per the vendor code) */
#define AXP_REG_STATUS2   0x01      /* bits [6:5] = 00 idle / 01 charging / 10 discharging */
#define AXP_REG_BAT_PCT   0xA4

/* Common config (0x10) bit 0 is soft power-off.
 * It used to show the words "power off" and stay on, which is worse than
 * doing nothing. It really cuts power now; PWR has to be pressed to return. */
#define AXP_REG_COMMON  0x10

void port_power_off(void)
{
    uint8_t v = 0;
    if (!axp_rd(AXP_REG_COMMON, &v)) return;
    ESP_LOGI("axp", "power off");
    axp_wr(AXP_REG_COMMON, v | 0x01);
}

int port_battery_percent(void)
{
    uint8_t st1 = 0, pct = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st1)) {
        ESP_LOGW("batt", "cannot read the PMU");
        return -1;
    }
    if (!(st1 & 0x08)) {
        ESP_LOGD("batt", "no battery (STATUS1=0x%02X)", st1);
        return -1;
    }
    if (!axp_rd(AXP_REG_BAT_PCT, &pct)) return -1;

    static bool logged;
    if (!logged) { logged = true; ESP_LOGI("batt", "STATUS1=0x%02X level=%d%%", st1, pct); }
    int p = pct > 100 ? 100 : pct;
    batt_track(p, (st1 & 0x20) != 0);
    return p;
}

/* ── time remaining ───────────────────────────────────────────
 * There is no capacity figure and no current sensor. So "how long one percent
 * takes to fall" is measured directly and multiplied by what is left — not an
 * invented number but one observed on this device.
 * It drains fast with the display on and slowly with it off, so the value
 * keeps moving, and the display says "at the current rate" for that reason. */
static int      s_last_pct = -1;
static int64_t  s_last_drop_us;
static float    s_min_per_pct;      /* minutes per percent; 0 means not known yet */

/* 🚨 The method is right — with no current sensor, timing how long a percent
 * takes is the thing to do. What was wrong was holding a single value.
 * Discharge curves are not straight, so the rate depends on the level. The
 * badge's own journal says so (186 lines):
 *     upper 100 -> 63%: 37% in 33,544 s = 15.1 min/%
 *     lower  62 ->  3%: 59% in 18,180 s =  5.1 min/%   <- three times faster
 * And with an EMA (0.7:0.3) running across charge boundaries, the bottom of
 * the last discharge follows into the top of the next. Recent measurement was
 * 16.4 min/% while the learned value was 11.0, and Settings showed "15h 46m"
 * at 86% — more than 30% short of the 23 hours the journal implies.
 * So the range is split into bands, learned separately, and the remaining
 * time is summed across them. */
#define BAND_N 3
static const uint8_t BAND_LO[BAND_N] = { 60, 30, 0 };   /* lower edge of each band */
static float s_mpp[BAND_N];                              /* minutes per percent, per band */
/* 🚨 Whether a band was actually learned. A "seed" copied from the old single
 * value into all three has to be distinguishable from a real measurement —
 * 30-59% and 0-29% both read 11.0 min/% and both were unmeasured seeds. The
 * same journal had a full discharge in it, and measured by hand that was
 * 7.9 min/%. **A value that was never measured must not look like one that
 * was.** Kept under a separate key: changing the mppb format would throw away
 * the 60-100% band that has been learned. */
static uint16_t s_mpp_seen[BAND_N];
static bool  s_mpp_loaded;

static int band_of(int pct)
{
    for (int b = 0; b < BAND_N; b++) if (pct >= BAND_LO[b]) return b;
    return BAND_N - 1;
}

static void mpp_load(void)
{
    if (s_mpp_loaded) return;
    s_mpp_loaded = true;
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READONLY, &nh) != ESP_OK) return;
    /* 🚨 The single overall value is restored here too. It used to be
     * **written to NVS and never read back**, so it started from zero on
     * every boot and the first percent drop after boot replaced the whole
     * accumulated average (the `== 0` branch). On a device plugged and
     * unplugged a dozen times a day that made it "the last single
     * measurement", which is why it swung between 23.1 and 38.6 hours. */
    uint32_t one = 0;
    if (nvs_get_u32(nh, "mpp", &one) == ESP_OK && one) s_min_per_pct = one / 100.0f;
    size_t nlen = sizeof(s_mpp_seen);
    if (nvs_get_blob(nh, "mppn", s_mpp_seen, &nlen) != ESP_OK || nlen != sizeof(s_mpp_seen))
        memset(s_mpp_seen, 0, sizeof(s_mpp_seen));
    size_t len = sizeof(s_mpp);
    if (nvs_get_blob(nh, "mppb", s_mpp, &len) != ESP_OK || len != sizeof(s_mpp)) {
        /* No per-band values yet. If there is an old single value, copy it
         * into all three so that upgrading does not drop back to "measuring".
         * They diverge from the next discharge onward. */
        uint32_t v = 0;
        if (nvs_get_u32(nh, "mpp", &v) == ESP_OK && v)
            for (int b = 0; b < BAND_N; b++) s_mpp[b] = v / 100.0f;
    }
    nvs_close(nh);
}

/* If that band has not been learned, borrow the nearest one that has. */
static float mpp_for(int band)
{
    if (s_mpp[band] > 0) return s_mpp[band];
    for (int d = 1; d < BAND_N; d++) {
        if (band - d >= 0     && s_mpp[band - d] > 0) return s_mpp[band - d];
        if (band + d < BAND_N && s_mpp[band + d] > 0) return s_mpp[band + d];
    }
    return 0;
}

static void batt_track(int pct, bool plugged)
{
    if (plugged) {                  /* no measuring while charging */
        s_last_pct = -1;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_pct < 0) { s_last_pct = pct; s_last_drop_us = now; return; }

    /* 🚨 The level went back up, which means the earlier low reading was not
     * energy spent but voltage sagging under load (the AXP2101's gauge works
     * from voltage; it is not a coulomb counter). On the board: 83% after
     * thirty minutes of recording, back to 88% when recording stopped.
     *
     * A rise used to be ignored, which left the reference at the sagged value
     * and counted the same stretch twice:
     *     90 -> (sag) 88 : recorded as 2% used
     *        -> (recover) 90 : ignored  <- not raising the reference here was the bug
     *        -> (real) 88 : recorded as another 2% used
     * So the learned rate came out faster than reality. A rise now resets the
     * reference. */
    if (pct > s_last_pct) {
            ESP_LOGI("batt", "level recovered %d%% -> %d%% — that was sag under load. Reference reset",
                 s_last_pct, pct);
        s_last_pct = pct;
        s_last_drop_us = now;
        return;
    }
    if (pct == s_last_pct) return;  /* has not dropped yet */

    int drop = s_last_pct - pct;
    float mins = (float)(now - s_last_drop_us) / 60000000.0f / drop;
    s_last_pct = pct;
    s_last_drop_us = now;
    if (mins < 0.2f || mins > 600.0f) return;      /* discard the impossible */

    /* Take the first one as-is and blend gently after that.
     * 🚨 Restore what was stored before blending, or the first drop after
     *    boot replaces what had accumulated. mpp_load is free to call twice. */
    mpp_load();
    s_min_per_pct = (s_min_per_pct == 0) ? mins : s_min_per_pct * 0.7f + mins * 0.3f;

    /* Put it in the band the level dropped into, so the gentle top and the
     * steep bottom do not contaminate each other. (Already loaded above.) */
    int b = band_of(pct);
    /* 🚨 The first real measurement over a seed replaces it rather than
     * blending. Otherwise an unmeasured value only moves 30% at a time and
     * keeps lying for a long while. */
    s_mpp[b] = (s_mpp[b] == 0 || s_mpp_seen[b] == 0) ? mins
                                                     : s_mpp[b] * 0.7f + mins * 0.3f;
    if (s_mpp_seen[b] < 65535) s_mpp_seen[b]++;

    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u32(nh, "mpp", (uint32_t)(s_min_per_pct * 100));
        nvs_set_blob(nh, "mppb", s_mpp, sizeof(s_mpp));
        nvs_set_blob(nh, "mppn", s_mpp_seen, sizeof(s_mpp_seen));
        nvs_commit(nh);
        nvs_close(nh);
    }
}

int port_battery_minutes_left(void)
{
    mpp_load();
    int pct = port_battery_percent();
    if (pct < 0) return -1;

    /* 🚨 Do not multiply the level by one number. Walk down percent by
     * percent to zero, adding that band's value — which comes out genuinely
     * shorter, because the bottom is steeper. */
    float total = 0;
    bool any = false;
    for (int p = pct; p > 0; p--) {
        float m = mpp_for(band_of(p));
        if (m <= 0) continue;
        total += m;
        any = true;
    }
    if (!any) return -1;
    return (int)total;
}

bool port_battery_charging(void)
{
    uint8_t st = 0;
    if (!axp_rd(AXP_REG_STATUS2, &st)) return false;
    return ((st >> 5) & 0x03) == 0x01;
}

/* At 100% charging finishes, so it is no longer "charging" — but it is still
 * plugged in and the indicator should say so. STATUS1 bit 5 is VBUS present. */
bool port_battery_plugged(void)
{
    uint8_t st = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st)) return false;
    return (st & 0x20) != 0;
}




/* ── the battery logger ────────────────────────────────────
 * The AXP2101 exposes no current sensor, only voltage (0x34/0x35, top five
 * bits plus low eight = mV). So there is no way to ask the chip how many
 * milliamps something costs. Instead, writing down the voltage and percentage
 * once a minute while using it builds the real discharge curve by itself — a
 * cheap way to turn an estimate into a measurement. */
#define AXP_REG_ADC_H 0x34
#define AXP_REG_ADC_L 0x35

int port_battery_mv(void)
{
    uint8_t h, l;
    if (!axp_rd(AXP_REG_ADC_H, &h) || !axp_rd(AXP_REG_ADC_L, &l)) return -1;
    int mv = ((h & 0x1F) << 8) | l;
    return mv > 0 ? mv : -1;
}

/* ── the battery journal ──────────────────────────────────────
 * Unplugged, there is nowhere for serial logs to go, so the badge writes this
 * into NVS itself. The interval widens below from one minute to five to
 * fifteen, so that 200 lines cover exactly one day (40 + 500 + 900 minutes).
 * 🚨 Which means the later part is at fifteen-minute spacing — do not assume
 *    a fixed interval when computing a slope.
 *
 * Plug it in in the morning and the boot log prints it as a table. */
#define JRN_MAX 200
/* 🚨 A uint16 `sec` overflows at 18.2 hours and cannot hold a day. uint32.
 * flags: bit0 = display on, bits 1-5 = what was happening (the port_crumb
 * value). That way normal use accumulates "which app costs what" for free. */
typedef struct { uint32_t sec; uint16_t mv; uint8_t pct; uint8_t flags; } jrn_t;
#define JRN_SCR(f)   ((f) & 1)
#define JRN_CRUMB(f) (((f) >> 1) & 0x1F)
/* Marks a break before this line — a charge or a reboot. A slope must never
 * be taken across this boundary (charging pushes the voltage back up and the
 * value inverts). */
#define JRN_BREAK    0x40
static jrn_t   s_jrn[JRN_MAX];
static uint16_t s_jrn_n;
static bool     s_jrn_loaded;

/* 🚨 Changing the journal's structure makes old NVS data read into the new
 * layout as garbage (it happened when sec went uint16 -> uint32 and scr ->
 * flags). A version number is stored alongside and mismatches are discarded.
 * Bump it whenever this struct changes. */
#define JRN_VER 2

static void jrn_load(void)
{
    if (s_jrn_loaded) return;
    s_jrn_loaded = true;
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READONLY, &nh) != ESP_OK) return;
    size_t len = sizeof(s_jrn);
    uint32_t n = 0, ver = 0;
    nvs_get_u32(nh, "jrnv", &ver);
    nvs_get_u32(nh, "jrnn", &n);
    if (ver != JRN_VER) { n = 0; ESP_LOGI("batt", "journal format changed — discarding the old one"); }
    if (n > JRN_MAX) n = 0;
    if (n && nvs_get_blob(nh, "jrn", s_jrn, &len) == ESP_OK) s_jrn_n = (uint16_t)n;
    nvs_close(nh);
}

/* One journal line. This used to live inside port_battery_log, and was pulled
 * out because the display on/off path has to write one too. */
static void jrn_put(int64_t now, int mv, int pct, bool scr, bool brk);

static void jrn_save(void)
{
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_set_blob(nh, "jrn", s_jrn, sizeof(jrn_t) * s_jrn_n);
    nvs_set_u32(nh, "jrnn", s_jrn_n);
    nvs_set_u32(nh, "jrnv", JRN_VER);
    nvs_commit(nh);
    nvs_close(nh);
}

/* ── the boot record ──────────────────────────────────────────
 * "Why did it reboot" is not answerable from the reason alone. What it was
 * doing just before, how long it had been up, and what the battery was at all
 * narrow it down, so they are recorded together.
 *
 * 🚨 This record never erases itself under any circumstances. Reading it
 * means plugging in, and if plugging in wiped it there would be no point (a
 * night of battery journal was lost that way once). Only a person clears it,
 * with badge-diag.sh --clear. */
#define BOOT_MAX 12
typedef struct {
    uint8_t  reason;    /* esp_reset_reason_t */
    uint8_t  pct;       /* battery % at boot */
    uint8_t  crumb;     /* what it was doing just before (port_crumb) */
    uint8_t  rep;       /* how many identical ones followed (0 = just the one) */
    uint32_t ran_sec;   /* how long it had been running before that */
} boot_rec_t;

static const char *CRUMB_NAME[] = {
    "just booted", "home", "lock", "screen off", "screen on",
    "opening app", "mouse", "meeting", "game", "calc", "clock", "settings", "recording",
};
#define CRUMB_N (sizeof(CRUMB_NAME)/sizeof(CRUMB_NAME[0]))

static uint8_t s_crumb;

/* Note what is happening now. Only called on transitions, so flash wear is negligible. */
int port_crumb_now(void) { return (int)s_crumb; }

void port_crumb(int what)
{
    if (what < 0 || what >= (int)CRUMB_N || what == s_crumb) return;

    /* 🚨 Two journal lines are written where an app changes: one under the old
     * app's name (closing its stretch) and one under the new (opening the
     * next). With only one line, one of the two apps loses its stretch
     * entirely. The two share a timestamp, so the gap between them falls out
     * of the slope calculation by itself (t <= 0 is discarded).
     * 🚨 Only while the display is on and the cable is out: off means there is
     * no app, and plugged in is not a discharge. A cooldown stops rapid app
     * switching from flooding the journal. */
    if (!port_battery_plugged() && !launcher_screen_is_off()) {
        static int64_t last_swap;
        int64_t now = esp_timer_get_time();
        if (!last_swap || now - last_swap >= 10000000LL) {
            int mv = port_battery_mv(), pct = port_battery_percent();
            if (mv > 0 && pct >= 0) {
                jrn_load();
                last_swap = now;
                jrn_put(now, mv, pct, true, false);          /* close under the old app */
                s_crumb = (uint8_t)what;
                jrn_put(now, mv, pct, true, false);          /* open under the new one */
            }
        }
    }
    s_crumb = (uint8_t)what;
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_set_u8(nh, "crumb", s_crumb);
    nvs_commit(nh);
    nvs_close(nh);
}

/* Called every 60 s while running. On the next boot this becomes "how long it was up". */
void port_uptime_mark(void)
{
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_set_u32(nh, "up", (uint32_t)(esp_timer_get_time() / 1000000));
    nvs_commit(nh);
    nvs_close(nh);
}

void port_reset_reason_note(int rr)
{
    static const char *RNAME[] = {
        "unknown","power on","external reset","software reset","panic","interrupt watchdog",
        "task watchdog","other watchdog","deep sleep wake","brownout","SDIO",
        "USB reset","JTAG reset","eFuse error","power glitch","CPU lockup",
    };
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) != ESP_OK) return;

    /* What the previous run left behind */
    uint32_t ran = 0; uint8_t crumb = 0;
    nvs_get_u32(nh, "up", &ran);
    nvs_get_u8(nh, "crumb", &crumb);

    boot_rec_t ring[BOOT_MAX] = {0};
    uint32_t n = 0;
    size_t len = sizeof(ring);
    nvs_get_u32(nh, "bootn", &n);
    if (n > BOOT_MAX) n = 0;
    if (n) nvs_get_blob(nh, "boot", ring, &len);

    boot_rec_t r = {
        .reason = (uint8_t)rr,
        .pct    = (uint8_t)(port_battery_percent() < 0 ? 0 : port_battery_percent()),
        .crumb  = crumb,
        .ran_sec = ran,
    };
    /* 🚨 Plugging in a cable to diagnose is itself a USB reset. Under sixty
     * seconds, port_uptime_mark never runs once and the entry reads "up for
     * 0 s". Those lines took half of a twelve-entry record (five of twelve) —
     * every read pushed a real record out by one. Having declared that the
     * record never erases itself, the act of reading it was eating it.
     * Identical consecutive entries are now counted rather than given a new
     * slot. Panics, watchdogs and brownouts have different reasons and do not
     * collapse together, so this is safe. */
    if (n && rr == ESP_RST_USB && ran == 0
        && ring[n - 1].reason == (uint8_t)rr && ring[n - 1].ran_sec == 0) {
        if (ring[n - 1].rep < 255) ring[n - 1].rep++;
        ring[n - 1].pct = r.pct;
    } else {
        if (n >= BOOT_MAX) {             /* push out the oldest */
            memmove(&ring[0], &ring[1], sizeof(boot_rec_t) * (BOOT_MAX - 1));
            n = BOOT_MAX - 1;
        }
        ring[n++] = r;
    }
    nvs_set_blob(nh, "boot", ring, sizeof(boot_rec_t) * n);
    nvs_set_u32(nh, "bootn", n);
    nvs_set_u32(nh, "up", 0);            /* a new stretch begins */
    nvs_commit(nh);
    nvs_close(nh);

    ESP_LOGI("rst", "─── boot record, %lu entries (most recent last) ───", (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        const char *rn = ring[i].reason < 16 ? RNAME[ring[i].reason] : "?";
        const char *cn = ring[i].crumb < CRUMB_N ? CRUMB_NAME[ring[i].crumb] : "?";
        char rep[28] = "";
        if (ring[i].rep)
            snprintf(rep, sizeof(rep), "  x%u (cable)", (unsigned)ring[i].rep + 1);
        ESP_LOGI("rst", "  %2lu) %-14s  up %5lu s before  battery %3u%%  doing=%s%s",
                 (unsigned long)i + 1, rn, (unsigned long)ring[i].ran_sec,
                 ring[i].pct, cn, rep);
    }
    ESP_LOGI("rst", "  note: panics or watchdogs repeating at 'screen on' point at the wake path");
}

void port_battery_journal_dump(void)
{
    /* This survives even if the journal does not — the observed "minutes per
     * percent", updated on every 1% drop while discharging and kept in NVS. */
    {
        nvs_handle_t nh;
        uint32_t v = 0;
        if (nvs_open("badge", NVS_READONLY, &nh) == ESP_OK) {
            nvs_get_u32(nh, "mpp", &v);
            nvs_close(nh);
        }
        uint32_t mah = 0;
        if (nvs_open("badge", NVS_READONLY, &nh) == ESP_OK) {
            nvs_get_u32(nh, "mah", &mah);
            nvs_close(nh);
        }
        if (mah) ESP_LOGI("batt", "measured battery capacity, about %lu mAh", (unsigned long)mah);
        if (v) {
            float mpp = v / 100.0f;
            ESP_LOGI("batt", "learned rate (single): %.1f min/%%  ->  %.1f h for 100%% (%.1f%%/h)",
                     mpp, mpp * 100.0f / 60.0f, 60.0f / mpp);
        } else {
            ESP_LOGI("batt", "no learned rate yet");
        }
        /* 🚨 The per-band values. Until they diverge, the remaining time is
         * wrong at both ends (it showed 15h46m at 86% while the journal said
         * 23 hours). Three values drifting apart means it is learning properly. */
        mpp_load();
        {
            char line[128]; int off = 0;
            for (int b = 0; b < BAND_N; b++) {
                int hi = (b == 0) ? 100 : BAND_LO[b - 1] - 1;
                off += snprintf(line + off, sizeof(line) - off, "%s%d~%d%%:",
                                b ? "  " : "", BAND_LO[b], hi);
                if (s_mpp[b] <= 0)
                    off += snprintf(line + off, sizeof(line) - off, "not yet");
                else if (s_mpp_seen[b] == 0)
                    /* Never measured. Say that it is borrowing the old single value. */
                    off += snprintf(line + off, sizeof(line) - off,
                                    "%.1f min/%%(borrowed)", s_mpp[b]);
                else
                    off += snprintf(line + off, sizeof(line) - off,
                                    "%.1f min/%%(%d samples)", s_mpp[b], s_mpp_seen[b]);
            }
            ESP_LOGI("batt", "per-band rate: %s", line);
        }
        {
            int left = port_battery_minutes_left();
            if (left > 0) ESP_LOGI("batt", "time left at this level: %dh %02dm (bands summed)",
                                   left / 60, left % 60);
        }
    }
    jrn_load();
    if (!s_jrn_n) { ESP_LOGI("batt", "journal empty"); return; }
    ESP_LOGI("batt", "─── battery journal, %u lines (display O/X) ───", s_jrn_n);
    for (uint16_t i = 0; i < s_jrn_n; i++) {
        uint8_t cb = JRN_CRUMB(s_jrn[i].flags);
        if (s_jrn[i].flags & JRN_BREAK)
            ESP_LOGI("batt", "  ──── break here (charged or rebooted) ────");
        ESP_LOGI("batt", "%6lu s  %4u mV  %3u%%  display %s  %s",
                 (unsigned long)s_jrn[i].sec, s_jrn[i].mv, s_jrn[i].pct,
                 JRN_SCR(s_jrn[i].flags) ? "O" : "X",
                 cb < CRUMB_N ? CRUMB_NAME[cb] : "?");
    }

    /* Voltage slope for display-on and display-off stretches, separately. The
     * difference between them is what the display costs. Percent only moves
     * every four or five minutes, which is too coarse, so this works in mV. */
    for (int mode = 0; mode < 2; mode++) {
        long dt = 0; long dv = 0; int seg = 0;
        for (uint16_t i = 1; i < s_jrn_n; i++) {
            if (s_jrn[i].flags & JRN_BREAK) continue;   /* across a charge or reboot the value inverts */
            if (JRN_SCR(s_jrn[i].flags) != mode || JRN_SCR(s_jrn[i-1].flags) != mode) continue;
            long t = (long)s_jrn[i].sec - (long)s_jrn[i-1].sec;
            int v = (int)s_jrn[i-1].mv - (int)s_jrn[i].mv;   /* how far it fell */
            if (t <= 0 || t > 1200) continue;                /* discard stretches broken by a reboot */
            dt += t; dv += v; seg++;
        }
        if (seg && dt > 0) {
            ESP_LOGI("batt", "── display %s: %ld mV over %ld s -> %.0f mV/h (%d samples)",
                     mode ? "O" : "X", dt, dv, dv * 3600.0 / dt, seg);
        } else {
            ESP_LOGI("batt", "── display %s: not enough samples", mode ? "O" : "X");
        }
    }
    /* Per-app consumption — display-on stretches only, split by what was
     * running. This is what answers "how many times more does the Clock app
     * cost than the lock screen".
     *
     * 🚨 These only accumulate if an app is used for **minutes at a time**.
     * Dipping in and out leaves every stretch under the threshold and prints
     * nothing — which is the correct behaviour. It is the limit of measuring
     * by voltage (this board has no coulomb counter). */
    #define APP_SEG_MIN 120   /* seconds. Anything shorter is a recovery curve; discard it */
    for (unsigned c = 0; c < CRUMB_N; c++) {
        long dt = 0, dv = 0; int seg = 0;
        for (uint16_t i = 1; i < s_jrn_n; i++) {
            if (s_jrn[i].flags & JRN_BREAK) continue;   /* across a charge or reboot the value inverts */
            /* 🚨 This used to require **both** lines to name the same app. But
             * a display-on stretch almost always ends with the app changing —
             * the lock screen on the way in, home on the way out — so nothing
             * ever matched. Nine display-on lines in the journal produced not
             * one per-app figure. A stretch is now credited to the app that
             * **started** it. */
            if (JRN_CRUMB(s_jrn[i-1].flags) != c) continue;
            if (!JRN_SCR(s_jrn[i].flags) || !JRN_SCR(s_jrn[i-1].flags)) continue;
            long t = (long)s_jrn[i].sec - (long)s_jrn[i-1].sec;
            long v = (long)s_jrn[i-1].mv - (long)s_jrn[i].mv;
            /* 🚨 A short stretch measures **voltage recovery**, not
             * consumption. When load drops the voltage climbs back up through
             * the internal resistance — the journal had a line 15 mV higher
             * after thirty seconds, which is a nonsense +1800 mV/h.
             * A handful of those inverts the total: [home] came out at
             * -33 mV/h and [lock screen] at 600, higher than a game. A black
             * clock face cannot cost more than a game.
             * Sag and recovery settle within tens of seconds, so only longer
             * stretches count. */
            if (t < APP_SEG_MIN || t > 1200) continue;
            dt += t; dv += v; seg++;
        }
        /* 🚨 Print nothing when the sample is thin. "Not known yet" beats a
         * wrong number, because people act on numbers. */
        if (seg >= 2 && dt >= 600)
            ESP_LOGI("batt", "── [%s] on for %ld min -> %.0f mV/h (%d samples)",
                     CRUMB_NAME[c], dt / 60, dv * 3600.0 / dt, seg);
    }

    /* 🚨 Measuring end to end crosses charge boundaries (both voltage and
     * percent go back up). Only from the last break onward — that is "the
     * stretch currently running". */
    uint16_t seg0 = 0;
    for (uint16_t i = s_jrn_n; i-- > 0; ) {
        if (s_jrn[i].flags & JRN_BREAK) { seg0 = i; break; }
    }
    if (s_jrn_n - seg0 >= 2) {
        long dsec = (long)s_jrn[s_jrn_n-1].sec - (long)s_jrn[seg0].sec;
        int dpct = (int)s_jrn[seg0].pct - (int)s_jrn[s_jrn_n-1].pct;
        if (dsec > 0 && dpct > 0)
            ESP_LOGI("batt", "── last stretch: %d%% in %ld min -> %.1f%%/h, %.1f h for 100%% (from line %u of %u)",
                     dsec / 60, dpct, dpct * 3600.0 / dsec, dsec * 100.0 / dpct / 3600.0,
                     s_jrn_n, (unsigned)(seg0 + 1));
    }
}

/* ── working out the battery capacity ─────────────────────────
 * Waveshare does not state the capacity anywhere (checked their own docs).
 * But the charge current is known, so it can be worked back from time:
 *     capacity (mAh) ~= charge current (mA) x hours / fraction filled
 * The percentage and time are captured when charging starts, and the sum is
 * done when it reaches 100% and written to NVS.
 * The current tapers during constant-voltage, so the answer comes out
 * slightly low — treat it as a lower bound. */
#define CHG_MA 200

static void charge_track(int pct, bool plugged)
{
    static bool     was;
    static int      start_pct;
    static int64_t  start_us;

    if (plugged && !was) {                 /* just plugged in */
        was = true; start_pct = pct; start_us = esp_timer_get_time();
        ESP_LOGI("axp", "charging from %d%% — timing to 100%% to get the capacity", pct);
        return;
    }
    if (!plugged) { was = false; return; }
    if (pct < 100 || start_pct < 0 || start_pct >= 95) return;

    float hours = (esp_timer_get_time() - start_us) / 3600000000.0f;
    float filled = (100 - start_pct) / 100.0f;
    if (hours < 0.15f || filled < 0.15f) { start_pct = -1; return; }
    int mah = (int)(CHG_MA * hours / filled);
    ESP_LOGI("axp", "capacity estimate %d mAh  (%d%% -> 100%%, %.2f h, %d mA)",
             mah, start_pct, hours, CHG_MA);
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u32(nh, "mah", (uint32_t)mah);
        nvs_commit(nh); nvs_close(nh);
    }
    start_pct = -1;                        /* once only */
}

void port_battery_log(const char *what)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (last && now - last < 25000000LL) return;   /* 25 s guard — the launcher calls every 60 */
    last = now;
    int mv = port_battery_mv();
    if (mv < 0) return;
    int pct = port_battery_percent();
    bool plug = port_battery_plugged();
    charge_track(pct, plug);
    ESP_LOGI("batt", "%s t=%llds %dmV %d%% %s", what ? what : "-",
             (long long)(now / 1000000), mv, pct, plug ? "(USB)" : "");

    /* 🚨 This used to erase the journal when charging started, but reading the
     * journal means plugging in — so plugging in destroyed a whole night of
     * measurements. It was self-defeating.
     * It is never erased now; a new discharge only opens a new stretch. */
    jrn_load();
    static int64_t last_jrn;
    static bool    was_plugged;
    if (plug) {
        was_plugged = true;
        return;                       /* while charging, neither write nor erase */
    }
    /* 🚨 It also used to wipe the journal on every unplug, so charging briefly
     * during the day destroyed the morning's record. Nothing is erased: a
     * "break here" mark is written and it continues. The slope calculation
     * reads that mark and refuses to cross it. */
    static bool mark_break;
    if (was_plugged) {
        was_plugged = false;
        mark_break = true;
        ESP_LOGI("batt", "cable out — marking a break and continuing");
    }
    if (!last_jrn) mark_break = true;   /* the first line after boot is a break too (seconds restart) */

    /* When it fills, drop the oldest rather than stopping. Losing today to
     * preserve yesterday has it backwards. */
    if (s_jrn_n >= JRN_MAX) {
        uint16_t drop = JRN_MAX / 4;
        memmove(s_jrn, s_jrn + drop, sizeof(jrn_t) * (JRN_MAX - drop));
        s_jrn_n = JRN_MAX - drop;
        if (s_jrn_n) s_jrn[0].flags |= JRN_BREAK;   /* the front was cut, so this is a break too */
    }
    /* The first 40 lines are a minute apart (for a quick slope), the next 100
     * five minutes, the rest fifteen. 200 lines cover exactly one day
     * (40 + 500 + 900 minutes = 24 hours). */
    int64_t gap = (s_jrn_n < 40)  ? 60000000LL
                : (s_jrn_n < 140) ? 300000000LL
                                  : 900000000LL;
    if (last_jrn && now - last_jrn < gap) return;
    last_jrn = now;
    if (pct >= 0) {
        jrn_put(now, mv, pct, !launcher_screen_is_off(), mark_break);
        mark_break = false;
    }
}

static void jrn_put(int64_t now, int mv, int pct, bool scr, bool brk)
{
    if (s_jrn_n >= JRN_MAX) return;
    s_jrn[s_jrn_n].sec = (uint32_t)(now / 1000000);
    s_jrn[s_jrn_n].mv  = (uint16_t)mv;
    s_jrn[s_jrn_n].pct = (uint8_t)pct;
    s_jrn[s_jrn_n].flags = (uint8_t)((scr ? 1 : 0)
                                     | ((s_crumb & 0x1F) << 1)
                                     | (brk ? JRN_BREAK : 0));
    s_jrn_n++;
    jrn_save();
}

/* 🚨 Consumption with the display on had never once been measured. The
 * journal writes on a timer (1 -> 5 -> 15 minutes) and the display sleeps
 * after thirty seconds, so two consecutive display-on lines could not happen.
 * A slope needs two neighbours both marked display-on — which is why 186
 * lines still reported "display O: not enough samples". The single largest
 * consumer was entirely outside the measurement.
 *
 * Now a line is written where it is switched on and again just before it goes
 * off. Those two are neighbours and both are display-on, so the stretch
 * between them is exactly the time the display was on.
 * 🚨 It has to be called *before* switching off. Called after, it records
 * display-off and the pair is broken. */
void port_battery_mark(bool screen_on)
{
    static int64_t last_mark;
    if (port_battery_plugged()) return;          /* plugged in is not a discharge */
    int64_t now = esp_timer_get_time();
    if (last_mark && now - last_mark < 15000000LL) return;   /* debounce flicker */
    int mv  = port_battery_mv();
    int pct = port_battery_percent();
    if (mv <= 0 || pct < 0) return;
    jrn_load();
    if (s_jrn_n >= JRN_MAX) return;              /* leave trimming to the normal path */
    last_mark = now;
    jrn_put(now, mv, pct, screen_on, false);
}

/* ── how much the CPU is actually awake ───────────────────────
 * Measuring with the battery means unplugging, but "what fraction of the time
 * the CPU is working" can be measured plugged in. More idle time with the
 * display off means less current — which is how to tell whether cutting a
 * poll actually helped.
 *
 * Two snapshots of per-task run time are subtracted to isolate the interval. */
#include "freertos/task.h"

#define CPU_MAX_TASKS 24
typedef struct { TaskHandle_t h; uint32_t rt; } cpu_snap_t;
static cpu_snap_t s_snap[CPU_MAX_TASKS];
static int        s_snap_n;
static uint32_t   s_snap_total;

void port_cpu_mark(void)
{
    TaskStatus_t st[CPU_MAX_TASKS];
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(st, CPU_MAX_TASKS, &total);
    s_snap_n = 0;
    for (UBaseType_t i = 0; i < n && i < CPU_MAX_TASKS; i++) {
        s_snap[s_snap_n].h  = st[i].xHandle;
        s_snap[s_snap_n].rt = st[i].ulRunTimeCounter;
        s_snap_n++;
    }
    s_snap_total = total;
}

void port_cpu_report(const char *when)
{
    TaskStatus_t st[CPU_MAX_TASKS];
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(st, CPU_MAX_TASKS, &total);
    uint32_t span = total - s_snap_total;
    if (!span) { ESP_LOGW("cpu", "%s: nothing measured", when); return; }

    uint32_t idle = 0;
    ESP_LOGI("cpu", "─── %s ───", when);
    for (UBaseType_t i = 0; i < n && i < CPU_MAX_TASKS; i++) {
        uint32_t prev = 0;
        for (int k = 0; k < s_snap_n; k++)
            if (s_snap[k].h == st[i].xHandle) { prev = s_snap[k].rt; break; }
        uint32_t d = st[i].ulRunTimeCounter - prev;
        int pct10 = (int)((uint64_t)d * 1000 / span);
        const char *nm = st[i].pcTaskName;
        if (strncmp(nm, "IDLE", 4) == 0) { idle += d; continue; }
        if (pct10 >= 3) ESP_LOGI("cpu", "  %-14s %4d.%d%%", nm, pct10 / 10, pct10 % 10);
    }
    /* Two cores, so idle time tops out at 200%. Normalised to 100. */
    int idle10 = (int)((uint64_t)idle * 1000 / span / 2);
    ESP_LOGI("cpu", "  idle %d.%d%%  (busy %d.%d%%)",
             idle10 / 10, idle10 % 10, (1000 - idle10) / 10, (1000 - idle10) % 10);
}


/* ── health checks ────────────────────────────────────────────
 * The lesson from one long night: "it did not crash" is the machine's
 * standard, and a person's is "does the display, the touch and the sound
 * work". The watchdog stayed quiet through thirteen thousand failed draws,
 * and that got called a pass — missing both the symptom and the cause.
 *
 * So there are two things here:
 *  1) count every error of any kind, including ones nobody thought of
 *  2) check directly that the outputs work (does the touch chip answer?) */
#include "esp_log.h"

static volatile uint32_t s_err_n, s_warn_n, s_err_known;
static vprintf_like_t    s_log_next;

/* 🚨 Counting alone is still only a symptom. Capture the "why" at the moment
 * it breaks. The failed draws turned out to be a memory problem, and a count
 * would never have shown that. Recording the resource state at the first
 * error puts symptom and cause on one line. */
static char     s_err_first[56];
static uint32_t s_err_free, s_err_big;
static uint8_t  s_err_ctx;          /* bit0 BLE connected, bit1 recording, bit2 display off */

/* Expand the format into the real sentence and look for a given phrase in it */
static bool fmt_has(const char *fmt, va_list ap, const char *needle)
{
    char line[160];
    va_list cp; va_copy(cp, ap);
    vsnprintf(line, sizeof line, fmt, cp);
    va_end(cp);
    return strstr(line, needle) != NULL;
}

static int log_hook(const char *fmt, va_list ap)
{
    if (fmt && fmt[0] == 'E') {
        /* 🚨 Harmless errors with a known cause are counted separately.
         * Mixing them in only inflates the count and hides real ones — but
         * they are not suppressed either. Only the known ones are set aside,
         * with the reason they are harmless written here.
         *
         *   The microphone codec handle covers transmit and receive as one
         *   object, so closing it also tries to disable the transmit channel
         *   that was never used (the log's "paired out_enable: 0"). The
         *   recording itself completes normally — all fifteen were present in
         *   the verification run. It is inside a vendor component and is left
         *   alone. */
        if (fmt_has(fmt, ap, "has not been enabled yet")) { s_err_known++; goto pass; }
        if (s_err_n++ == 0) {
            /* 🚨 Copying only the format string leaves "E (%lu) %s: %s(",
             * which is useless (an entire night was spent unable to see the
             * cause because of that). va_copy expands it safely so the real
             * sentence is kept. */
            va_list cp;
            va_copy(cp, ap);
            vsnprintf(s_err_first, sizeof s_err_first, fmt, cp);
            va_end(cp);
            s_err_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            s_err_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            s_err_ctx  = (uint8_t)((port_hid_connected() ? 1 : 0)
                                 | (port_rec_active()   ? 2 : 0)
                                 | (launcher_screen_is_off() ? 4 : 0));
        }
    } else if (fmt && fmt[0] == 'W') s_warn_n++;
pass:
    return s_log_next ? s_log_next(fmt, ap) : 0;
}

void port_health_begin(void)
{
    if (!s_log_next) s_log_next = esp_log_set_vprintf(log_hook);
    s_err_n = s_warn_n = s_err_known = 0;
    s_err_first[0] = 0;
}

/* Are the things a person sees actually alive? Zero means everything is fine. */
int port_health_check(char *out, size_t len)
{
    int bad = 0;
    char tp[24] = "unknown";

    /* Is the touch chip alive? Just whether it acknowledges over I2C.
     * 🚨 This used to read the check-code register (0xD1FC) and compare
     * against 0x204ECACA, but the value came back different every time
     * (0x00200232 = x32 y562, i.e. coordinates). The chip is in report mode
     * and hands back touch data instead of the register asked for. Taking the
     * lock made no difference, so it was not a race — the protocol was simply
     * misunderstood. That value cannot be trusted, so only the certain thing
     * is checked: with the power gone or the chip dead there is no ACK. */
    port_lock();
    tp_open();
    if (s_tp) {
        uint8_t reg[2] = { 0xD1, 0xFC };
        if (i2c_master_transmit(s_tp, reg, 2, 200) == ESP_OK) {
            snprintf(tp, sizeof tp, s_tp_asleep ? "asleep" : "responding");
        } else if (s_tp_asleep) {
            snprintf(tp, sizeof tp, "asleep");     /* no answer while asleep is correct */
        } else { snprintf(tp, sizeof tp, "no answer"); bad++; }
    }

    port_unlock();

    if (s_err_n) bad++;

    if (s_err_n) {
        /* Put the symptom (which error) and the cause (what the resources were) on one line */
        char *nl = strchr(s_err_first, '\n'); if (nl) *nl = 0;
        snprintf(out, len, "%lu errors, touch %s | first \"%s\" with internal %luKB, largest block %luKB%s%s%s",
                 (unsigned long)s_err_n, tp, s_err_first,
                 (unsigned long)(s_err_free / 1024), (unsigned long)(s_err_big / 1024),
                 (s_err_ctx & 1) ? " BLE-connected" : "",
                 (s_err_ctx & 2) ? " recording" : "",
                 (s_err_ctx & 4) ? " display-off" : "");
    } else {
        snprintf(out, len, "ok, %lu warnings%s, touch %s", (unsigned long)s_warn_n,
                 s_err_known ? ", harmless (explained)" : "", tp);
    }
    return bad;
}

uint32_t port_health_errors(void) { return s_err_n; }
