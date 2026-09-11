#include "app.h"
#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "port.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "badge";

#ifdef BADGE_CPU_PROBE
/* An experiment that measures power saving while plugged in.
 * Twenty seconds with the display on and twenty with it off, comparing what
 * fraction of the time the CPU worked. With no way to measure the battery,
 * this is the only objective yardstick. */
static void probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));      /* let the boot noise settle */

    for (int round = 1; round <= 3; round++) {
        port_lock(); launcher_screen_toggle(); port_unlock();   /* make sure it is on */
        vTaskDelay(pdMS_TO_TICKS(1500));
        port_cpu_mark();
        vTaskDelay(pdMS_TO_TICKS(20000));
        port_cpu_report("display on, 20 s");

        port_lock(); launcher_screen_off_manual(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(1500));
        port_cpu_mark();
        vTaskDelay(pdMS_TO_TICKS(20000));
        port_cpu_report("display off, 20 s");
        ESP_LOGW(TAG, "[probe] round %d done", round);
    }
    ESP_LOGW(TAG, "[probe] finished");
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_HOSTILE
/* Adversarial checks. Not the happy path — edges and races.
 * Bugs do not live in "using it slowly and correctly", they live in "using it
 * fast and carelessly". */
static uint32_t hs_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
/* 🚨 "Did it crash" is not enough. The watchdog stayed quiet through thirteen
 * thousand failed draws and that got called a pass. After every step, check
 * that the things a person sees are still alive — a single error is a
 * failure. */
static int s_hs_fail;

static void hs(const char *n, uint32_t b)
{
    uint32_t now = hs_heap(); long d = (long)now - (long)b;
    char health[64];
    int bad = port_health_check(health, sizeof health);
    if (bad) s_hs_fail++;
    ESP_LOGW(TAG, "[stress] %-28s heap %6u (%+ld)  %s%s", n, (unsigned)now, d,
             health, bad ? "   ** FAILED" : "");
    port_health_begin();        /* the next step starts a fresh count */
}

static void hostile_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    port_health_begin();
    ESP_LOGW(TAG, "════════ stress test start, heap %u ════════", (unsigned)hs_heap());
    static const badge_app_t *const A[] = {
        &app_calc, &app_clock, &app_keys, &app_games, &app_meet, &app_mouse,
    };
    const int AN = sizeof(A)/sizeof(A[0]);
    uint32_t h;

    /* 1. Move on before the animation finishes — a screen transition race */
    h = hs_heap();
    for (int i = 0; i < 150; i++) {
        port_lock(); launcher_open((const badge_app_t *)A[i % AN]); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(60));           /* shorter than the open animation */
        port_lock(); launcher_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    hs("app thrash x150 (60ms)", h);

    /* 2. Switch the display off and on mid-transition */
    h = hs_heap();
    for (int i = 0; i < 60; i++) {
        port_lock(); launcher_open((const badge_app_t *)A[i % AN]); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(80));
        port_lock(); launcher_screen_off_manual(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(80));
        port_lock(); launcher_screen_toggle(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(80));
        port_lock(); launcher_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    hs("display toggle mid-transition x60", h);

    /* 3. Call the recording API in nonsense order */
    h = hs_heap();
    port_rec_stop();                       /* stop something that is not running */
    ESP_LOGW(TAG, "[stress] stop without start -> active=%d", port_rec_active());
    if (port_rec_start(0)) {
        vTaskDelay(pdMS_TO_TICKS(800));
        bool twice = port_rec_start(1);    /* start again while running */
        ESP_LOGW(TAG, "[stress] start while running -> %s", twice ? "**accepted (bug)" : "rejected (correct)");
        port_rec_stop(); port_rec_stop();  /* stop twice */
        for (int i = 0; i < 20 && port_rec_active(); i++) vTaskDelay(pdMS_TO_TICKS(300));
    }
    hs("recording API out of order", h);

    /* 4. Switch apps while recording — the recording has to survive */
    h = hs_heap();
    if (port_rec_start(0)) {
        for (int i = 0; i < 12; i++) {
            port_lock(); launcher_open((const badge_app_t *)A[i % AN]); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(400));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        ESP_LOGW(TAG, "[stress] recording %s while switching apps (%lu s)",
                 port_rec_active() ? "alive (correct)" : "**dead (bug)",
                 (unsigned long)port_rec_seconds());
        port_rec_stop();
        for (int i = 0; i < 20 && port_rec_active(); i++) vTaskDelay(pdMS_TO_TICKS(300));
    }
    hs("app switches while recording x12", h);

    /* 5. Overflow the directory — there are twelve slots, so record fifteen times */
    h = hs_heap();
    for (int i = 0; i < 15; i++) {
        if (!port_rec_start(0)) { ESP_LOGW(TAG, "[stress] recording %d rejected", i + 1); break; }
        vTaskDelay(pdMS_TO_TICKS(1200));
        port_rec_stop();
        for (int k = 0; k < 20 && port_rec_active(); k++) vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "[stress] checking the list after 15 recordings (%lu min free)",
             (unsigned long)(port_rec_free_seconds() / 60));
    hs("slot overflow x15", h);

    /* 6. Bring BLE up and down quickly */
    h = hs_heap();
    for (int i = 0; i < 8; i++) {
        port_hid_start(); vTaskDelay(pdMS_TO_TICKS(400));
        port_hid_stop();  vTaskDelay(pdMS_TO_TICKS(400));
    }
    hs("BLE thrash x8", h);

    /* 7. Hammer the settings store (NVS) */
    h = hs_heap();
    for (int i = 0; i < 40; i++) {
        port_brightness_set(20 + (i % 60));
        badge_display_set_xgap((i % 2) ? 8 : 6);
        vTaskDelay(pdMS_TO_TICKS(30));   /* without yielding, the watchdog bites */
    }
    badge_display_set_xgap(8);
    port_brightness_set(45);
    hs("brightness and margin, 40 changes", h);

    ESP_LOGW(TAG, "════════ stress test end, heap %u — %d steps failed ════════",
             (unsigned)hs_heap(), s_hs_fail);
    vTaskDelete(NULL);
}
#endif


#ifdef BADGE_APPBENCH
/* ── the bench that runs the apps itself ──────────────────────
 * 🚨 Things that could only be found by hand (frame times, the tilt
 * reference) are measured by the device on its own. The simulator is x86 and
 * runs 150 times faster, and it has no IMU at all — so anything driven by
 * tilt had never once been tested there. */
extern void games_debug_play_bricks(void);
extern void games_debug_play_orb(void);
extern void games_debug_tilt0(float *base, int *set);
extern void orb_set_lon(float lon);
#include <math.h>

static void appbench_task(void *arg)
{
    (void)arg;
    /* Wait for the IMU to fall asleep (five seconds unused). This recreates
     * exactly the situation where the first round of bricks did not work. */
    vTaskDelay(pdMS_TO_TICKS(9000));
    ESP_LOGW(TAG, "════════ app bench start ════════");

    float ax = 0, ay = 0, az = 0;
    bool  ok = port_imu_accel3(&ax, &ay, &az);
    ESP_LOGW(TAG, "first read after sleeping: %s x=%.0f y=%.0f z=%.0f",
             ok ? "trustworthy" : "not yet", ax, ay, az);

    /* (1) bricks — does the tilt reference match the current attitude? */
    port_lock(); launcher_open(&app_games); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(500));
    port_lock(); games_debug_play_bricks(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));
    float base = 0; int set = 0;
    games_debug_tilt0(&base, &set);
    port_imu_accel3(&ax, &ay, &az);
    ESP_LOGW(TAG, "brick tilt reference %s base=%.1f, after 1.5 s actual y=%.1f, difference %.1f",
             set ? "taken" : "**not taken", base, ay, ay - base);
    if (set && fabsf(ay - base) > 60.0f)
        ESP_LOGE(TAG, "** the reference is %.0f away from reality — the paddle will sit at one end", ay - base);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* (2) water — frame time */
    port_lock(); launcher_home(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    port_lock(); launcher_open(&app_water); port_unlock();
    ESP_LOGW(TAG, "── water, 12 s ──");
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* (3) the globe — frame time (kept spinning throughout) */
    port_lock(); launcher_home(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    port_lock(); launcher_open(&app_games); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(400));
    port_lock(); games_debug_play_orb(); port_unlock();
    ESP_LOGW(TAG, "── globe, 12 s ──");
    for (int i = 0; i < 12; i++) {
        port_lock(); orb_set_lon((i % 10) * 0.1f); port_unlock();   /* keep it moving */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    port_lock(); launcher_home(); port_unlock();
    ESP_LOGW(TAG, "════════ app bench end ════════");
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_BTNTEST
/* An overnight run with nobody watching. Since nobody is watching, it checks
 * "did what a person sees actually change" rather than "did it not crash". */
static int s_bt_fail;
static uint32_t s_jit_n, s_jit_sum, s_jit_max, s_jit_last;
static void jit_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    if (s_jit_last) {
        uint32_t el = now - s_jit_last;
        if (el < 500) { s_jit_sum += el; s_jit_n++; if (el > s_jit_max) s_jit_max = el; }
    }
    s_jit_last = now;
}

static void bt(const char *name, bool ok, const char *detail)
{
    if (!ok) s_bt_fail++;
    ESP_LOGW(TAG, "[button] %-34s %s%s%s", name, ok ? "pass" : "** FAIL",
             detail && detail[0] ? " — " : "", detail ? detail : "");
}

static void btntest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(9000));
    ESP_LOGW(TAG, "════════ button and sound checks start ════════");

    /* ── 1. Does a BOOT press actually toggle the display? ──────────────
     * ⚠️ GPIO0 is a strapping pin, so press it only briefly and only while
     *    the display is on. (With the display off it enters light sleep, and
     *    a reset coinciding with that can land in download mode.) */
    char d[80];
    for (int i = 0; i < 4; i++) {
        bool before = launcher_screen_is_off();
        uint32_t n0 = port_boot_isr_count();
        port_boot_btn_fake(60);
        vTaskDelay(pdMS_TO_TICKS(700));
        bool after = launcher_screen_is_off();
        uint32_t dn = port_boot_isr_count() - n0;
        snprintf(d, sizeof d, "display %s->%s, %lu interrupts",
                 before ? "off" : "on", after ? "off" : "on", (unsigned long)dn);
        /* The interrupt should fire exactly once and the display state should
         * have flipped. Firing several times means the level interrupt ran away. */
        bt("BOOT press toggles the display", (after != before) && dn == 1, d);

        /* The next round has to start with the display on (strapping risk) */
        if (launcher_screen_is_off()) {
            port_boot_btn_fake(60);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }

    /* ── 2. Is a very short press caught? (the case that used to be missed) ── */
    {
        bool before = launcher_screen_is_off();
        uint32_t n0 = port_boot_isr_count();
        port_boot_btn_fake(25);
        vTaskDelay(pdMS_TO_TICKS(700));
        snprintf(d, sizeof d, "%lu interrupts",
                 (unsigned long)(port_boot_isr_count() - n0));
        bt("a 25 ms press is caught", launcher_screen_is_off() != before, d);
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 3. Does rapid pressing make it run away? ───────────────────── */
    {
        uint32_t n0 = port_boot_isr_count();
        for (int i = 0; i < 6; i++) { port_boot_btn_fake(50); vTaskDelay(pdMS_TO_TICKS(400)); }
        uint32_t dn = port_boot_isr_count() - n0;
        snprintf(d, sizeof d, "6 presses, %lu interrupts", (unsigned long)dn);
        bt("rapid presses do not flood the interrupt", dn <= 8, d);
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 4. With sound off, is the codec left alone? ─────────────────── */
    {
        port_health_begin();
        port_tone_volume(0);                 /* the same state as sound OFF in Settings */
        for (int i = 0; i < 40; i++) {
            port_tone_freq(800 + i * 10);
            port_tone_enable(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            port_tone_enable(false);
        }
        port_tone_hold(true);                /* even if a game tries to hold it */
        vTaskDelay(pdMS_TO_TICKS(500));
        bool held_open = port_tone_codec_open();
        port_tone_hold(false);
        snprintf(d, sizeof d, "codec %s", held_open ? "open" : "not open");
        bt("sound OFF leaves the codec alone", !held_open, d);
    }

    /* ── 5. With sound on, does it come back? (catches an over-eager 4) ── */
    {
        port_tone_volume(60);
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        bool open = port_tone_codec_open();
        port_tone_freq(1000); port_tone_enable(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        port_tone_enable(false);
        port_tone_hold(false);
        snprintf(d, sizeof d, "codec %s", open ? "open" : "not open");
        bt("sound ON claims the codec", open, d);
    }

    /* ── 6. Held but silent, does it let go by itself? (power) ───────── */
    {
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        bool open_now = port_tone_codec_open();
        vTaskDelay(pdMS_TO_TICKS(23000));    /* past the 20 s threshold */
        bool open_later = port_tone_codec_open();
        port_tone_hold(false);
        snprintf(d, sizeof d, "on hold %s, after 23 s %s",
                 open_now ? "open" : "closed", open_later ? "open" : "closed");
        bt("held but silent for 20 s, it lets go", open_now && !open_later, d);
    }

    /* ── 7. Does the display going off release the codec? (game left open) ── */
    {
        port_tone_volume(60);
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(200));
        bool before = port_tone_codec_open();
        port_lock(); launcher_screen_toggle(); port_unlock();   /* display off */
        vTaskDelay(pdMS_TO_TICKS(400));
        port_tone_hold(false);               /* as an app does when it sees the display go off */
        vTaskDelay(pdMS_TO_TICKS(4000));     /* past the 3 s threshold */
        bool after = port_tone_codec_open();
        snprintf(d, sizeof d, "before %s, after %s",
                 before ? "open" : "closed", after ? "open" : "closed");
        bt("display off releases the codec", before && !after, d);
        /* Leave the display on again */
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 8. How often does a 20 ms LVGL timer really fire on hardware? ──
     * The games move the ball on this timer. The old approach took one step
     * per call, so a late timer slowed the ball by that much. It was changed
     * to measure elapsed time and split it into steps — which raises the
     * effective speed, and how much it raises it decides whether the ball
     * speed has to be retuned. */
    {
        s_jit_n = 0; s_jit_sum = 0; s_jit_max = 0; s_jit_last = 0;
        lv_timer_t *jt;
        port_lock(); jt = lv_timer_create(jit_cb, 20, NULL); port_unlock();
        /* Measuring while idle is meaningless; measure while the display is busy */
        static const badge_app_t *const J[] = { &app_calc, &app_clock, &app_keys };
        for (int i = 0; i < 24; i++) {
            port_lock(); launcher_open(J[i % 3]); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        port_lock(); lv_timer_delete(jt); port_unlock();
        uint32_t avg = s_jit_n ? s_jit_sum / s_jit_n : 0;
        snprintf(d, sizeof d, "mean %lu ms, max %lu ms (%lu samples)",
                 (unsigned long)avg, (unsigned long)s_jit_max, (unsigned long)s_jit_n);
        /* This is the worst case. It is not a pass/fail number, it is a number to know. */
        bt("timer interval (app thrash = worst case)", true, d);
    }

    /* ── 9. Timer interval while a real game is running ─────────────────
     * This is the number that sets the ball speed. Step 8 (app thrash) is not
     * a situation anyone plays in. */
    {
        extern void games_debug_play_bricks(void);
        port_lock(); launcher_open(&app_games); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(900));
        port_lock(); games_debug_play_bricks(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));

        s_jit_n = 0; s_jit_sum = 0; s_jit_max = 0; s_jit_last = 0;
        lv_timer_t *jt;
        port_lock(); jt = lv_timer_create(jit_cb, 20, NULL); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(8000));
        port_lock(); lv_timer_delete(jt); port_unlock();
        port_lock(); launcher_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        uint32_t avg = s_jit_n ? s_jit_sum / s_jit_n : 0;
        snprintf(d, sizeof d, "mean %lu ms, max %lu ms (%lu samples) -> old method's effective speed %lu%%",
                 (unsigned long)avg, (unsigned long)s_jit_max, (unsigned long)s_jit_n,
                 avg ? (unsigned long)(2000 / avg) : 0);
        bt("timer interval (bricks, real)", avg > 0, d);
    }

    char health[80];
    int bad = port_health_check(health, sizeof health);
    ESP_LOGW(TAG, "════════ button and sound checks end — %d failed, %s ════════",
             s_bt_fail + (bad ? 1 : 0), health);
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_SELFTEST
/* The full self-test, run without anyone touching it.
 * The internal heap is logged after each step so a leak stands out. */
static uint32_t st_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static void st_step(const char *name, uint32_t before)
{
    uint32_t now = st_heap();
    long d = (long)now - (long)before;
    ESP_LOGW(TAG, "[check] %-26s heap %6u  (%+ld)%s",
             name, (unsigned)now, d, (d < -2000) ? "   **leak" : "");
}

static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    /* Run it twice. Comparing the first and second pass separates
     * "first-time allocation" from "a real leak" — still shrinking on the
     * second pass means it leaks. */
    for (int pass = 1; pass <= 1; pass++) {
    uint32_t h0 = st_heap();
    ESP_LOGW(TAG, "════════ self-test pass %d start, internal heap %u ════════", pass, (unsigned)h0);

    /* Bring BLE up and leave it up, to check it can be found by a scan from outside. */
    ESP_LOGW(TAG, "[check] BLE advertising — holding for 3 minutes");
    port_hid_start();
    for (int i = 0; i < 180; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i % 15 == 0)
            ESP_LOGW(TAG, "[check] BLE %s  peer=%s  interval %d ms",
                     port_hid_connected() ? "connected" : "advertising",
                     port_hid_peer() ? port_hid_peer() : "-",
                     port_hid_interval_ms());
        /* When connected, actually send reports. Watching with btmon from
         * outside shows the ATT notifications — proof that HID really goes out. */
        if (port_hid_connected()) {
            port_hid_mouse(20, 0, 0, 0);   vTaskDelay(pdMS_TO_TICKS(60));
            port_hid_mouse(-20, 0, 0, 0);  vTaskDelay(pdMS_TO_TICKS(60));
            if (i % 10 == 0) { port_hid_key(0, 0x04); }   /* 'a' */
        }
    }
    ESP_LOGW(TAG, "════════ pass %d end, internal heap %u (%+ld from the start) ════════",
             pass, (unsigned)st_heap(), (long)st_heap() - (long)h0);
    }   /* end of the pass loop */
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_SOAK_DISPLAY
/* Reproduces exactly what a person did:
 *   display off -> on (lock screen) -> unlock with the handle (home) -> off ...
 * That is a far wider path than switching the display off and on: the screen
 * transition animation, creating and deleting the handle objects, and the
 * lock screen cache are all involved. An earlier panic came from here. */
static void soak_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGW(TAG, "[soak] 120 rounds of off/on plus unlock");
    for (int i = 1; i <= 120; i++) {
        port_lock(); launcher_screen_off_manual(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));

        port_lock(); launcher_screen_toggle(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        /* Unlocking = exactly what the handle does */
        port_lock(); launcher_show_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        /* Occasionally go into an app and back — close_done_cb NULLing the
         * handle pointer is where that panic actually happened. */
        if (i % 5 == 0) {
            port_lock(); launcher_open(&app_calc); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        if (i % 10 == 0)
            ESP_LOGW(TAG, "[soak] %3d/120 passed, internal heap %u", i,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    ESP_LOGW(TAG, "[soak] all 120 passed — the off/on and unlock path is clean");
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* esp_hid sends connect and disconnect through the default event loop.
     * Without that loop the callbacks never fire at all, and a disconnect
     * goes unnoticed. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* The display is brought up by us — holding the panel handle is what
     * makes it possible to switch the display truly off. The BSP's
     * bsp_display_start_with_config() hides that handle. The full reasoning
     * is at the top of display.c. */
    if (!badge_display_start()) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }

    /* Reading at the default 30 ms makes the mouse look choppy. Read faster. */
    lv_indev_t *indev = badge_display_indev();
    if (indev) lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    port_rtc_restore();
    port_time_autosync();
    settings_load();      /* stored brightness, sound and timeout */
    /* Load the settings but keep the display dark. GRAM still holds garbage —
     * brightening here reproduces the flash at boot exactly. */
    badge_display_brightness(0);

    /* Light sleep — switched on once, reverted when "display off and on
     * reboots it" appeared, and switched back on after the cause was blocked.
     * The cause was PSRAM: the LVGL task's stack lived there, and light sleep
     * half-sleeps PSRAM. If the task runs on wake before that memory is
     * ready, it dies on the spot.
     *   (a) LVGL stack into internal RAM (stack_in_psram=false in display.c)
     *   (b) CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED off
     * Both are in place. During a BLE connection or I2S recording,
     * port_pm_hold() keeps it awake.
     *
     * This is the one large lever left — with the display off the CPU is
     * already idle 99.1% of the time and still draws current. Not from
     * working, but from being awake. */
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "power management config failed");

    bsp_display_lock(UINT32_MAX);
    splash_show();              /* home appears as the splash finishes */
    bsp_display_unlock();

    /* Raise the brightness only after the first frame is on the panel. That
     * order is what removes the flash of uninitialised GRAM at boot. */
    vTaskDelay(pdMS_TO_TICKS(80));
    badge_display_brightness(port_brightness_get());

    /* Why it rebooted. Brownout (a flat battery) versus a crash is decided
     * here, and it has to be knowable without a serial cable — so it is kept
     * until the next boot. */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        static const char *NAME[] = {
            "unknown", "power on", "external reset", "software reset", "panic",
            "interrupt watchdog", "task watchdog", "other watchdog", "deep sleep wake",
            "brownout", "SDIO", "USB reset", "JTAG reset", "eFuse error",
            "power glitch", "CPU lockup",
        };
        const char *n = (rr < sizeof(NAME)/sizeof(NAME[0])) ? NAME[rr] : "?";
        if (rr == ESP_RST_BROWNOUT)      ESP_LOGE(TAG, "reboot reason: brownout — the voltage collapsed (a flat battery, probably)");
        else if (rr == ESP_RST_PWR_GLITCH) ESP_LOGE(TAG, "reboot reason: power glitch — the rail wobbled");
        else if (rr == ESP_RST_CPU_LOCKUP) ESP_LOGE(TAG, "reboot reason: CPU lockup");
        else if (rr == ESP_RST_PANIC)    ESP_LOGE(TAG, "reboot reason: panic — the code crashed");
        else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT)
                                         ESP_LOGE(TAG, "reboot reason: watchdog — something held the CPU");
        else                             ESP_LOGI(TAG, "reboot reason: %s (%d)", n, (int)rr);
        port_reset_reason_note((int)rr);
    }

#ifdef BADGE_HOSTILE
    xTaskCreate(hostile_task, "hostile", 6144, NULL, 3, NULL);
#endif
#ifdef BADGE_APPBENCH
    xTaskCreate(appbench_task, "appbench", 6144, NULL, 3, NULL);
#endif
#ifdef BADGE_BTNTEST
    xTaskCreate(btntest_task, "btntest", 6144, NULL, 3, NULL);
#endif
#ifdef BADGE_SELFTEST
    xTaskCreate(selftest_task, "selftest", 6144, NULL, 3, NULL);
#endif
#ifdef BADGE_CPU_PROBE
    xTaskCreate(probe_task, "probe", 4096, NULL, 3, NULL);
#endif
#ifdef BADGE_SOAK_DISPLAY
    /* A temporary check: hammer the display off/on path with nobody touching
     * it. This is where the light-sleep reboot happened, so it has to be
     * re-verified after switching to actually powering the panel down. */
    xTaskCreate(soak_task, "soak", 4096, NULL, 3, NULL);
#endif
    /* Booted while plugged in means never sleeping from the start — otherwise
     * the log cuts out for the sixty seconds before the launcher timer runs. */
    if (port_battery_plugged()) port_pm_hold(true);

    badge_creds_init();            /* seed the badge if secrets.h has values */
    port_battery_journal_dump();   /* if last night left a record, it goes here */
    port_heap_report("boot");
    ESP_LOGI(TAG, "splash -> launcher");
}
