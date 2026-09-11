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

/* BOOT 버튼 = GPIO0. 런타임엔 일반 버튼으로 쓸 수 있다.
 * PWR은 GPIO가 아니라 AXP2101 PWRON이라 여기서 못 쓴다. */
#define HOME_BTN_GPIO   GPIO_NUM_0

static volatile bool s_boot_hit;    /* 인터럽트가 걸어두는 눌림 */
/* 레벨 인터럽트라 누르고 있는 동안 계속 불린다 → 잡자마자 꺼둔다.
 * 다시 켜는 건 손을 뗀 걸 확인한 뒤 아래 태스크가 한다. */
static volatile uint32_t s_boot_isr_n;   /* 인터럽트가 실제로 물었나 세는 값 */
static void IRAM_ATTR boot_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(HOME_BTN_GPIO);
    s_boot_isr_n++;
    s_boot_hit = true;
}

uint32_t port_boot_isr_count(void) { return s_boot_isr_n; }

/* 🚨 BOOT(GPIO0) 를 소프트웨어로 눌러본다. 오픈드레인이라 코드는 LOW 로만
 * 끌 수 있고 HIGH 로는 못 민다 — 사람이 동시에 눌러도 서로 안 싸운다.
 * ⚠️ GPIO0 은 부팅 모드 스트랩 핀이다. 낮게 잡고 있는 동안 리셋이 나면
 *    다운로드 모드로 들어간다. 그래서 짧게만, 그리고 화면이 켜져 있어
 *    라이트슬립에 안 들어간 동안에만 쓴다. 시험용 외엔 부르지 않는다. */
void port_boot_btn_fake(uint32_t ms)
{
    /* 🚨 gpio_config() 를 쓰면 안 된다 — 그 핀의 인터럽트 설정까지 통째로
     * 초기화해서, 시험 도구가 시험 대상을 꺼버린다(0908 에 실제로 그랬다:
     * 첫 눌림만 인터럽트가 물고 그 뒤론 0회). 방향만 바꾼다. */
    gpio_set_direction(HOME_BTN_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(HOME_BTN_GPIO, 0);            /* 누른다 */
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(HOME_BTN_GPIO, 1);            /* 뗀다(풀업이 올린다) */
    gpio_set_direction(HOME_BTN_GPIO, GPIO_MODE_INPUT);
}
#define KV_NS           "badge"

void port_lock(void)
{
    /* 0 은 "기다리지 말고 바로 실패"다. LVGL 태스크가 쥐고 있으면 락 없이
     * 그림을 그리게 된다 — 부팅 로그에 그 에러가 찍혔다. 기다리게 바꾼다. */
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        ESP_LOGW("port", "LVGL 락 실패");
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
/* 마지막으로 세운 밝기. BSP 는 이걸 안 기억하고 켤 때마다 100% 로 올린다. */
static int s_bright_saved = 45;

static void rtc_write_now(void);
static void time_synced_now(void);

/* SNTP 가 실제로 시각을 넣었을 때만 불린다. 이 깃발이 유일한 성공 증거다. */
static volatile bool s_sntp_done;
static void sntp_got_time(struct timeval *tv) { (void)tv; s_sntp_done = true; }

/* ── 라이트슬립 잠금 ──────────────────────────────────────────
 * 켜두면 할 일이 없을 때 CPU 가 잔다(대기 전류가 크게 준다). 다만 BLE 연결과
 * I2S 녹음은 자는 동안 끊길 수 있어서, 그 둘이 도는 동안만 못 자게 잡는다.
 * 여러 곳에서 잡을 수 있으니 세어서 마지막이 놓을 때 풀린다. */
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

static void home_btn_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HOME_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* 🚨 화면이 꺼지면 300ms 마다만 본다. 톡 누르고 떼는 데 100~200ms 라
     * 눌린 순간이 확인과 확인 사이에 통째로 들어가 안 보였다.
     * 게다가 그 사이엔 CPU 가 라이트슬립에 들어가 있어서, 그냥 인터럽트만
     * 걸어놔서는 깨우지도 못한다. 라이트슬립에서 깨우려면 '레벨' 이어야
     * 한다(엣지는 안 된다) — 그래서 눌림(LOW) 을 깨움 조건으로 준다. */
    /* 🚨 이미 누가 설치했으면 IDF 가 E 로그를 찍고 INVALID_STATE 를 준다.
     * 우리한텐 정상인 경우인데 오류로 남아, 진짜 오류를 찾을 때 방해가 된다
     * (0908 판정 도구가 이걸 잡아냈다). 이 한 번만 조용히 부른다. */
    esp_log_level_t lv = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);
    esp_err_t ie = gpio_install_isr_service(0);
    esp_log_level_set("gpio", lv);
    if (ie != ESP_OK && ie != ESP_ERR_INVALID_STATE)
        ESP_LOGW("btn", "인터럽트 서비스 실패 %s — BOOT 는 폴링으로만 잡힌다", esp_err_to_name(ie));
    gpio_isr_handler_add(HOME_BTN_GPIO, boot_isr, NULL);
    gpio_wakeup_enable(HOME_BTN_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    gpio_intr_enable(HOME_BTN_GPIO);

    /* 짧게 = 홈, 1초 이상 = 화면 끄기 */
    axp_init();

    /* 🚨 화면이 꺼졌을 때 300ms 마다 본 게 문제였다. 톡 누르고 떼는 데
     * 100~200ms 라 눌림이 확인과 확인 사이에 통째로 들어갔다.
     * 그렇다고 전부 빨리 보면 안 된다 — 비싼 건 GPIO 가 아니라 AXP 다(I2C).
     * 그래서 둘을 나눈다: 핀은 100ms 마다(공짜), AXP 는 300ms 마다(그대로).
     * 인터럽트가 잡아주면 그보다 먼저 깨지만, 못 잡아도 100ms 면 놓치지 않는다. */
    const int SLICE_ON = 40, SLICE_OFF = 100;
    int prev = 1, held = 0, axp_acc = 0;
    bool fired = false;
    while (1) {
        bool off = launcher_screen_is_off();
        int slice = off ? SLICE_OFF : SLICE_ON;

        axp_acc += slice;
        if (!off || axp_acc >= 300) {
            axp_acc = 0;
            /* PWR 짧게 = 홈. 전원 버튼이 아무 일도 안 하면 아무도 못 찾는다. */
            int pk = port_pwr_key();
            if (pk == 1) {
                port_lock(); launcher_home(); port_unlock();
            } else if (pk == 2) {
                /* 안내를 잠깐 보여주고 실제로 끊는다 */
                port_lock(); launcher_poweroff_notice(); port_unlock();
                vTaskDelay(pdMS_TO_TICKS(700));
                port_power_off();
            }
        }

        if (s_boot_hit) {
            s_boot_hit = false;
            /* 자고 있을 때 핀이 깨운 것인지 로그로 남긴다. 이건 소프트웨어로는
             * 만들어낼 수 없는 상황이라(누르는 주체가 CPU 다) 실물 눌림에서만
             * 확인된다 — 그래서 확인할 수 있게 적어둔다. */
            esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
            ESP_LOGI("btn", "BOOT 눌림 [인터럽트] (직전에 깨운 것: %s)",
                     wc == ESP_SLEEP_WAKEUP_GPIO  ? "핀"
                   : wc == ESP_SLEEP_WAKEUP_TIMER ? "타이머"
                   : wc == ESP_SLEEP_WAKEUP_UNDEFINED ? "안 잤음" : "기타");
            if (s_on_press) { port_lock(); s_on_press(); port_unlock(); }
            /* 손을 뗄 때까지 기다렸다가 인터럽트를 되살린다. 누른 채로 켜면
             * 레벨 인터럽트가 쉴 새 없이 다시 걸린다. */
            for (int i = 0; i < 60 && gpio_get_level(HOME_BTN_GPIO) == 0; i++)
                vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(60));       /* 튐 방지 */
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
            /* 인터럽트가 못 잡았을 때를 위한 폴링 경로 */
            if (prev == 0 && !fired && s_on_press) {
                ESP_LOGI("btn", "BOOT 눌림 [폴링] — 인터럽트가 못 잡았다");
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
    /* 뒤집어 쓰니 BOOT 가 손에 잡히는 자리다. 화면 켜고 끄기를 여기로 옮겼다.
     * PWR 짧게 = 홈. PWR 길게 = 전원 차단(하드웨어라 못 바꾼다). */
    (void)on_press;
    s_on_press = launcher_screen_toggle;
    s_on_hold  = NULL;
    xTaskCreate(home_btn_task, "home_btn", 3072, NULL, 4, NULL);
}

void port_radio_set(int need)
{
    /* TODO: BLE HID on/off. WiFi와 안테나를 공유하므로 동시에 켜지 않는다. */
    ESP_LOGI("radio", "need=%d", need);
}

/* ── 에뮬레이터 지원 ─────────────────────────────────────────── */
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
    /* 1ms 넘으면 태스크를 재우고, 짧으면 그냥 돌린다.
     * 다마고치 CPU는 32768Hz라 한 사이클이 30us — 재울 만큼 길지 않다. */
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        us %= 1000;
    }
    if (us) esp_rom_delay_us(us);
}

/* 뻗는 게 메모리 때문인지 추측으로 못 정한다. 숫자를 남긴다.
 * 내부 RAM 은 512KB 뿐이고 BLE 스택이 크게 먹는다. PSRAM 은 8MB 라 넉넉하다.
 * 큰 덩어리(largest_free_block)가 총량보다 훨씬 작아지면 조각남이다. */
void port_heap_report(const char *when)
{
    size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t i_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("heap", "%-10s 내부 %uKB(최대덩어리 %uKB, 최저 %uKB) PSRAM %uKB",
             when, (unsigned)(i_free / 1024), (unsigned)(i_big / 1024),
             (unsigned)(i_min / 1024), (unsigned)(p_free / 1024));

    if (i_free < 24 * 1024) ESP_LOGW("heap", "내부 RAM 이 바닥나고 있다");
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

/* ── 소리 ────────────────────────────────────────────────────
 * 다마고치는 "몇 Hz를 켜라/꺼라"가 전부다. 사각파 하나면 그 시절 소리가 난다.
 * ES8311 코덱에 I2S로 밀어넣는다. */
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
static volatile bool     s_tone_held;   /* 소리 쓰는 앱이 켜져 있나 */

static void tone_task(void *arg)
{
    (void)arg;
    static int16_t buf[TONE_CHUNK];
    uint32_t phase = 0;

    while (1) {
        if (!s_tone_on || !s_spk) {
            phase = 0;
            /* 3초 조용하면 코덱을 닫아 버퍼를 돌려준다 */
            /* 🚨 소리를 낼 때마다 코덱을 열면 첫 소리가 통째로 안 들린다
             * (0908 실기: 게임에서 절전 뒤 첫 효과음이 빠지고 다음 것부터 남).
             * 그렇다고 소리 쓰는 앱이 켜진 동안 무한정 붙잡으면 절전이
             * 무너진다. 붙잡은 동안엔 문턱만 늘린다 — 20초 조용하면 그때는
             * 놀고 있는 게 아니니 닫는다. */
            int64_t idle = s_tone_held ? 20000000 : 3000000;
            if (s_codec_open && (tone_muted() || esp_timer_get_time() - s_tone_last_us > idle))
                codec_close();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t hz = s_tone_hz ? s_tone_hz : 1000;
        uint32_t period = TONE_SR / hz;          /* 한 주기의 샘플 수 */
        if (period < 2) period = 2;
        int16_t amp = (int16_t)(9000 * s_tone_vol / 100);

        for (int i = 0; i < TONE_CHUNK; i++) {
            buf[i] = (phase < period / 2) ? amp : -amp;
            if (++phase >= period) phase = 0;
        }
        esp_err_t we = esp_codec_dev_write(s_spk, buf, sizeof(buf));
    if (we != ESP_OK) { static int c; if (c++ % 50 == 0) ESP_LOGE("tone", "★ 쓰기 실패 %s", esp_err_to_name(we)); }
    }
}

void port_tone_init(void)
{
    if (s_spk) return;
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGW("tone", "스피커 초기화 실패 — 소리 없이 간다");
        return;
    }
    xTaskCreate(tone_task, "tone", 3072, NULL, 6, NULL);   /* 코덱은 필요할 때 연다 */
}

void port_tone_freq(uint32_t hz)  { s_tone_hz = hz; }
/* 코덱을 열어두면 I2S 버퍼가 계속 잡혀 있다. 소리를 안 낸 지 3초가 지나면
 * 닫고, 필요할 때 다시 연다. 여는 데 얼마나 걸리는지는 로그로 남긴다. */
static void codec_open(void)
{
    if (s_codec_open || !s_spk) return;
    int64_t t0 = esp_timer_get_time();
    esp_codec_dev_sample_info_t fs = { .sample_rate = TONE_SR, .channel = 1, .bits_per_sample = 16 };
    esp_err_t oe = esp_codec_dev_open(s_spk, &fs);
    if (oe != ESP_OK) {
        /* 🚨 열기 실패인데 열렸다고 적어두면, 나중에 닫을 때 안 연 채널을
         * 끄려 해서 i2s_channel_disable 오류가 난다(자가검사에서 15회). */
        ESP_LOGW("tone", "스피커 열기 실패 %s — 녹음이 I2S 를 쓰는 중. 소리만 건너뛴다", esp_err_to_name(oe));
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, s_tone_vol);
    s_codec_open = true;
    ESP_LOGI("tone", "코덱 열기 %lld ms", (esp_timer_get_time() - t0) / 1000);
}

static void codec_close(void)
{
    if (!s_codec_open || !s_spk) return;
    esp_codec_dev_close(s_spk);
    s_codec_open = false;
}

void port_tone_enable(bool on)
{
    /* 🚨 예전엔 port_tone_init() 을 아무도 안 불러서 s_spk 가 NULL 이었고,
     * codec_open() 이 조용히 되돌아가 소리가 한 번도 안 났다(0907 에 발견).
     * 다마고치를 지울 때 초기화 호출까지 같이 날아간 것으로 보인다.
     * 처음 소리를 낼 때 알아서 올린다 — 안 쓰면 코덱도 안 잡는다. */
    /* 🚨 꺼놨는데도 코덱을 잡던 것을 막는다(0908 지적). */
    if (on && tone_muted()) { s_tone_on = false; return; }
    if (on && !s_spk) port_tone_init();
    if (on) { codec_open(); s_tone_last_us = esp_timer_get_time(); }
    s_tone_on = on;
}
/* 소리를 끄면 볼륨만 0 이 된다 — 그런데 코덱은 그대로 열려 무음을 밀고
 * 있었다(I2S 도 돌고 앰프도 켜진 채). 안 들릴 소리를 내느라 전기를 쓴 셈이다.
 * 볼륨 0 = 아예 건드리지 않는다. */
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
        /* 끄는 순간 이미 열려 있던 것도 놓는다. 다음 소리를 기다릴 이유가 없다. */
        s_tone_on = false;
        s_tone_held = false;
        return;                 /* 닫는 건 tone 태스크가 한다(같은 데서만 만진다) */
    }
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_tone_vol);
}

void port_brightness_set(int percent)
{
    if (percent < 1) percent = 1;      /* 0 은 "꺼짐"이라 설정으로 못 가게 */
    if (percent > 100) percent = 100;
    s_bright_saved = percent;
    badge_display_brightness(percent);
}
int  port_brightness_get(void)        { return s_bright_saved; }

/* ── 시각 맞추기 ──────────────────────────────────────────────
 * WiFi를 켜서 SNTP로 맞추고 바로 끈다. 상시 접속할 이유가 없고,
 * WiFi와 BLE는 안테나를 나눠 쓰기 때문에 켜두면 마우스가 굼떠진다. */
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
/* 여러 곳을 오가면 여기에 더 적는다. 안 적으면 그 칸은 없는 것으로 친다. */
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

/* ── 접속정보는 배지에 저장해 둔다 ──────────────────────────
 * 🚨 코드에 박아 두면, secrets.h 가 없는 컴퓨터(회사 등)에서 구웠을 때
 * 배지가 WiFi 를 통째로 잃는다. 게다가 예전 예제엔 "여기에 SSID" 같은
 * 글자가 있어서 그걸로 esp_wifi_set_config 를 부르면 **배지에 저장돼
 * 있던 멀쩡한 접속정보까지 덮어썼다**(0909 지적).
 *
 * NVS 는 구워도 살아남는다. 그러니 여기 한 번 넣어두고 그걸 쓴다.
 * secrets.h 에 값이 있으면 구울 때 새로 넣고(집에서 굽는 경우),
 * 비어 있으면 저장된 것을 그대로 쓴다(회사에서 굽는 경우). */
#define WIFI_NS  "badge"
/* WIFI_SLOTS 는 port.h 가 정한다 — 화면 쪽도 같은 값을 봐야 한다 */

/* 🚨 예전엔 secrets.h 값이 배지에 저장된 것을 **매 부팅마다 덮었다.** 화면에서
 * WiFi 를 넣을 방법이 없던 시절엔 그게 맞았는데, 키패드가 생긴 뒤로는 화면에서
 * 넣은 것이 재부팅하면 secrets.h 값으로 되돌아간다(0911 제보: "실제로 안 붙은
 * 것 같은데"). **비어 있을 때만 넣는다.**
 * 그래도 원래 목적은 그대로다 — 새 배지에 처음 구우면 씨앗이 들어가고,
 * secrets.h 가 빈 컴퓨터에서 구워도 배지에 있던 것이 안 지워진다.
 * 🚨 secrets.h 를 고쳐 다시 넣고 싶으면 화면에서 그 칸을 비우고 구워라. */
static void creds_seed(const char *key, const char *val)
{
    if (!val || !val[0]) return;             /* 비었으면 건드리지 않는다 */
    nvs_handle_t nh;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &nh) != ESP_OK) return;
    char old[64] = "";
    size_t n = sizeof old;
    if (nvs_get_str(nh, key, old, &n) == ESP_OK && old[0]) {
        nvs_close(nh);                       /* 이미 들어 있다 — 안 건드린다 */
        return;
    }
    nvs_set_str(nh, key, val);
    nvs_commit(nh);
    ESP_LOGI("wifi", "%s 를 배지에 처음 넣었다", key);
    nvs_close(nh);
}

/* 저장된 값을 준다. 없으면 빈 문자열. */
static void creds_get(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t nh;
    if (nvs_open(WIFI_NS, NVS_READONLY, &nh) != ESP_OK) return;
    size_t n = cap;
    if (nvs_get_str(nh, key, out, &n) != ESP_OK) out[0] = '\0';
    nvs_close(nh);
}

/* 🚨 1번 칸은 옛 키 이름을 그대로 쓴다("wifi_ssid"). 이름을 바꾸면 배지에
 * 이미 들어 있는 집 WiFi 가 사라진다 — NVS 는 구워도 살아남는 게 요점인데
 * 키를 갈아버리면 그 요점이 무너진다. */
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

/* 한 칸이라도 채워져 있나 */
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


/* 🚨 WiFi 를 올린 **뒤에** 부른다. 우리 칸이 비어 있으면 WiFi 스택이 지난번에
 * 저장해 둔 것을 가져다 쓴다 — 그래야 "집에서 한 번 구워 씨를 뿌린 다음에만
 * 회사에서 구울 수 있다" 는 순서 의존이 없어진다. 가져왔으면 우리 칸에도
 * 적어둔다.
 * ⏳ 스택이 저장해 둔 것을 정말 돌려주는지는 **기기에서 안 재봤다**(0909,
 *    배지를 들고 나감). 안 되면 로그에 "가져올 것도 없다" 가 찍힌다. */
/* 저장된 것 중 **지금 실제로 잡히는** 것을 고른다.
 *
 * 🚨 순서대로 붙어보는 방식은 쓰지 않았다. 없는 망에 붙으려다 실패하는 데
 * 한 번에 5~10초가 날아가서, 밖에 있을 때마다 집 WiFi 를 먼저 기다리게 된다.
 * 한 번 훑는 데는 1~2초면 되고 헛된 시도가 없어 결과적으로 더 빠르다.
 *
 * 🚨 훑으려면 WiFi 가 이미 올라와 있어야 한다(esp_wifi_start 뒤에 부를 것).
 * 그래서 접속정보를 고르는 자리가 esp_wifi_start **뒤로** 옮겨졌다 —
 * 예전엔 올리기 전에 골랐다. */
/* ── 화면에서 부르는 훑기 ──────────────────────────────────── */
static wifi_found_t  s_scan[WIFI_SCAN_MAX];
static volatile int  s_scan_n = -1;      /* -1 = 도는 중 */
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
                            if (!ap[i].ssid[0]) continue;   /* 숨긴 망은 고를 수 없다 */
                            snprintf(s_scan[found].ssid, sizeof s_scan[found].ssid,
                                     "%s", (const char *)ap[i].ssid);
                            s_scan[found].rssi = ap[i].rssi;
                            /* 이미 저장된 것인지 표시해 준다 */
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
    ESP_LOGI("wifi", "훑기 끝 — %d개", found);
    s_scan_n = found;
    s_scan_busy = false;
    vTaskDelete(NULL);
}

void port_wifi_scan_start(void)
{
    if (s_scan_busy) return;
    s_scan_busy = true;
    s_scan_n = -1;
    xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL);
}

int port_wifi_scan_result(wifi_found_t *out, int max)
{
    int n = s_scan_n;
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, s_scan, n * sizeof(wifi_found_t));
    return n;
}

/* ── 붙어보기 ──────────────────────────────────────────────── */
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
                /* 🚨 '붙었다' 의 기준은 주소를 받은 것이다. 링크만 붙고 DHCP 가
                 * 안 되면 아무것도 못 한다 — 거기까지 봐야 진짜다. */
                esp_netif_ip_info_t ip = { 0 };
                for (int i = 0; i < 30; i++) {          /* 최대 15초 */
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
    ESP_LOGI("wifi", "붙어보기 %s (SSID %s)",
             ok == WIFI_TRY_OK ? "성공" : "실패", s_try_ssid);
    memset(s_try_pass, 0, sizeof s_try_pass);   /* 오래 들고 있을 이유가 없다 */
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
        /* 저장된 것을 쓴다 — 이미 넣어둔 망을 다시 칠 이유가 없다. */
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
    xTaskCreate(try_task, "wifitry", 4096, NULL, 4, NULL);
}

int port_wifi_try_state(void) { return s_try_state; }

void port_wifi_last_ok(char *ssid, size_t ss)
{
    creds_get("wifi_last", ssid, ss);
}

/* ── 칸에 넣고 지우기 ──────────────────────────────────────── */
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
    ESP_LOGI("wifi", "%d번 칸에 넣었다 (SSID %s)", slot + 1, ssid ? ssid : "");
}

void port_wifi_slot_clear(int slot)
{
    if (slot < 0 || slot >= WIFI_SLOTS) return;
    slot_write(slot, "wifi_ssid", NULL);
    slot_write(slot, "wifi_pass", NULL);
    ESP_LOGI("wifi", "%d번 칸을 비웠다", slot + 1);
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
    /* 🚨 다 찼으면 마지막 칸을 민다. 1번은 secrets.h 씨앗이 들어가는 자리라
     * 되도록 남긴다 — 그게 없으면 새 배지가 아무 데도 못 붙는다. */
    return WIFI_SLOTS - 1;
}

bool port_wifi_slot_get(int slot, char *ssid, size_t ss)
{
    char p2[65];
    if (slot < 0 || slot >= WIFI_SLOTS) { if (ss) ssid[0] = '\0'; return false; }
    /* 🚨 비밀번호는 받아만 오고 버린다. 화면으로 내보내지 않는다. */
    bool ok = creds_wifi_slot(slot, ssid, ss, p2, sizeof p2);
    memset(p2, 0, sizeof p2);
    return ok;
}

bool badge_wifi_pick(char *ssid, size_t ss, char *pass, size_t ps)
{
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW("wifi", "훑기 실패 — 1번 칸으로 그냥 간다");
        return badge_creds_wifi_live(ssid, ss, pass, ps);
    }
    uint16_t got = 0;
    esp_wifi_scan_get_ap_num(&got);
    /* 🚨 내부 RAM 이 얇다(BLE 가 떠 있으면 20KB 대). 한 칸이 80바이트쯤이라
     * 16개면 1.3KB — 이 정도로 끊는다. 신호 센 것부터 오므로 손해가 적다. */
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
        ESP_LOGI("wifi", "훑어보니 %d개 — %s 로 간다 (%ddBm, %d번 칸)",
                 (int)got, bs, best_rssi, best + 1);
        snprintf(ssid, ss, "%s", bs);
        snprintf(pass, ps, "%s", bp);
        return true;
    }
    /* 🚨 하나도 안 잡혀도 포기하진 않는다 — 숨긴 망이거나 훑는 순간에만
     * 안 보였을 수 있다. 1번 칸으로 한 번은 시도한다. */
    ESP_LOGW("wifi", "훑은 %d개 중 아는 게 없다 — 1번 칸으로 시도", (int)got);
    return badge_creds_wifi_live(ssid, ss, pass, ps);
}

bool badge_creds_wifi_live(char *ssid, size_t ss, char *pass, size_t ps)
{
    if (badge_creds_wifi(ssid, ss, pass, ps)) return true;
    wifi_config_t wc = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK || !wc.sta.ssid[0]) {
        ESP_LOGW("wifi", "배지에 접속정보가 없고 가져올 것도 없다");
        return false;
    }
    snprintf(ssid, ss, "%s", (const char *)wc.sta.ssid);
    snprintf(pass, ps, "%s", (const char *)wc.sta.password);
    creds_seed("wifi_ssid", ssid);
    creds_seed("wifi_pass", pass);
    ESP_LOGI("wifi", "스택이 저장해 둔 접속정보를 가져왔다 (SSID %s)", ssid);
    return true;
}

static volatile net_state_t s_net = NET_IDLE;

/* 🚨 예전엔 `#if !defined(BADGE_WIFI_SSID)` 로 갈랐다. 이제 접속정보는
 * 컴파일 때가 아니라 배지에 있으므로, 있는지 없는지는 돌면서 봐야 한다. */
/* 🚨 esp_netif_create_default_wifi_sta() 는 두 번 부르면 assert 로 뻗는다
 * (0907 자가검사에서 두 번째 업로드 때 잡혔다). 시각 동기와 녹음 업로드가
 * 둘 다 WiFi 를 쓰므로 여기 한 곳에서만 만들고 나눠 쓴다. 만든 netif 는
 * 지우지 않는다 — 지웠다 다시 만드는 것도 같은 함정이다. */
/* 🚨 WiFi 는 한 번에 하나만 쓴다.
 * 시각 동기(timesync)와 녹음 업로드(recup)가 각자 esp_wifi_init 을 부르는데,
 * 겹치면 같은 하드웨어를 두 번 올리게 된다. 부팅 때 시각 동기가 늦어지고
 * 30초 업로드 폴링이 겹치면 실제로 만나는 상황이다.
 * 먼저 잡은 쪽이 쓰고 나머지는 물러난다 — 어차피 둘 다 나중에 다시 온다. */
static SemaphoreHandle_t s_wifi_gate;

bool badge_wifi_take(uint32_t wait_ms)
{
    if (!s_wifi_gate) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
        if (!s_wifi_gate) s_wifi_gate = xSemaphoreCreateMutex();
        portEXIT_CRITICAL(&mux);
    }
    if (!s_wifi_gate) return true;          /* 만들지 못했으면 막지 않는다 */
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
        esp_event_loop_create_default();      /* 이미 있으면 조용히 실패한다 */
        nif = esp_netif_create_default_wifi_sta();
    }
    return nif;
}

static void sync_task(void *arg)
{
    (void)arg;
    if (!badge_wifi_take(1000)) {           /* 업로드가 쓰는 중이면 물러난다 */
        ESP_LOGI("net", "WiFi 를 다른 쪽이 쓰는 중 — 시각 동기는 다음에");
        s_net = NET_IDLE;
        vTaskDelete(NULL);
        return;
    }
    s_net = NET_CONNECTING;
    char ss[33] = "", pw[65] = "";

    badge_wifi_netif_once();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    /* 🚨 못 올렸는데 내리려 들면 0x3001 오류만 두 줄 남고 원인은 안 보인다.
     * 여기서 이유를 남기고 물러난다. */
    esp_err_t we = esp_wifi_init(&ic);
    if (we != ESP_OK) {
        ESP_LOGE("net", "★ WiFi 못 올림 %s — 시각 맞추기 포기", esp_err_to_name(we));
        s_net = NET_FAIL;
        badge_wifi_give();
        vTaskDelete(NULL);
        return;
    }
    /* 🚨 훑으려면 먼저 올려야 한다. 그래서 올리고 → 고르고 → 붙는다. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    if (!badge_wifi_pick(ss, sizeof ss, pw, sizeof pw)) {
        ESP_LOGW("net", "WiFi 접속정보가 없다 — 시각 맞추기 건너뜀");
        s_net = NET_NOCONF;
        esp_wifi_stop();
        esp_wifi_deinit();
        badge_wifi_give();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI("net", "시각 맞추기 시작 (SSID %s)", ss);
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ss);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pw);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI("net", "WiFi 접속 시도 중");

    /* 🚨 예전엔 "시각이 2023년 이후면 맞은 것" 으로 성공을 판정했다. 그런데
     * 부팅할 때 이미 (틀린) 시각이 들어 있으면 **첫 검사에서 곧바로 참**이 된다.
     * NTP 가 답하기도 전에 성공으로 치고 SNTP 를 꺼버렸다 — 맨 처음 한 번
     * (시계가 1970년일 때)만 진짜로 작동하고 그 뒤론 계속 헛돌았다.
     * 0910 에 폰보다 8~9분 빠른 채로 몇 번을 다시 구워도 안 고쳐지던 게 이것이다.
     * 🚨 "시각이 없을 때만 참인 조건" 을 성공 판정으로 쓰지 마라 — 한 번
     * 성공하고 나면 그 뒤로 아무 일도 안 하면서 성공했다고 답한다.
     * SNTP 가 실제로 값을 넣었을 때만 부르는 콜백을 쓴다. */
    s_sntp_done = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_got_time);
    esp_sntp_init();

    time_t before = time(NULL);
    /* 최대 20초 기다린다. 안 되면 포기하고 라디오를 끈다. */
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_sntp_done) break;
    }
    s_net = s_sntp_done ? NET_SYNCED : NET_FAIL;
    if (s_sntp_done) {
        /* 얼마나 틀어져 있었는지 남긴다 — 잠든 시간을 잘못 세는 만큼이
         * 여기 그대로 찍힌다. 재동기 주기를 정하는 근거가 된다. */
        long off = (long)(time(NULL) - before);
        ESP_LOGI("net", "시각 맞췄다 (%+ld초 어긋나 있었다)", off);
    }
    if (s_net == NET_SYNCED) {
        rtc_write_now();          /* 전원 끊겨도 살아남게 */
        time_synced_now();        /* 다음 재동기 시계를 여기서 다시 센다 */
    }

    esp_sntp_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI("net", "시각 맞추기 %s", s_net == NET_SYNCED ? "성공" : "실패");
    badge_wifi_give();
    vTaskDelete(NULL);
}

void port_time_sync_start(void)
{
    /* 배지에 접속정보가 없으면 나갈 데가 없다 */
    if (!badge_creds_wifi_any()) { s_net = NET_NOCONF; return; }
    if (s_net == NET_CONNECTING) return;
    xTaskCreate(sync_task, "timesync", 4096, NULL, 4, NULL);
}

/* 이 보드엔 RTC 칩이 없다 — 전원이 끊기면 시각이 사라진다.
 * 그래서 부팅하면 알아서 한 번 맞추러 나간다. */
/* 🚨 예전엔 "시각이 안 맞춰졌을 때만" 맞췄다. 그래서 한 번 맞추고 나면
 * **영영 다시 안 맞췄다** — 이 보드엔 RTC 칩이 없어 시각이 칩 발진기로만
 * 흐르는데, 그게 하루에 몇 분씩 앞선다(0910 제보: 폰보다 3~4분 빠름).
 * 맞춰져 있어도 주기적으로 다시 맞춘다. */
/* 🚨 6시간은 모자랐다. 이 칩은 자는 동안 흐른 시간을 **내부 RC 발진기**로
 * 재는데(CONFIG_RTC_CLK_SRC_INT_RC), 그게 1% 남짓 틀린다. 배지는 화면이
 * 꺼지면 거의 내내 라이트슬립이라(PM_ENABLE + TICKLESS_IDLE) 그 오차가 그대로
 * 쌓인다 — 0910 에 반나절 만에 8분 앞섰다(8분 / 12시간 = 1.1%, 딱 RC 오차다).
 * 크리스털 드리프트가 아니라 **잠든 시간을 잘못 세는 것**이라 발진기를 바꾸지
 * 않는 한 안 없어진다. 자주 맞춰 오차를 가둔다 — 1시간이면 40초 안쪽이다.
 * 한 번 맞추는 값은 WiFi 8초 남짓(0.3mAh 정도)이라 대기 소모에 묻힌다.
 * ⏭ 근본 처방은 32.768kHz 크리스털을 쓰는 것이다(20ppm). 보드에 그 부품이
 *    있는지 확인이 먼저다 — 없으면 IDF 가 로그를 남기고 RC 로 되돌아간다. */
#define RESYNC_SEC  (1 * 3600)
static time_t s_last_sync;

static void time_synced_now(void) { s_last_sync = time(NULL); }

void port_time_autosync(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) { port_time_sync_start(); return; }
    /* 부팅 직후 s_last_sync 가 0 이면 '오래됐다' 로 보고 한 번 맞춘다 */
    if (s_last_sync == 0 || now - s_last_sync > RESYNC_SEC) port_time_sync_start();
}
net_state_t port_time_sync_state(void) { return s_net; }

const char *port_bt_status(void)
{
    return "off";        /* BLE HID 붙이면 연결된 호스트 이름을 돌려준다 */
}

/* ── 손가락 수 ────────────────────────────────────────────────
 * BSP 는 터치 핸들을 감춰두지만, LVGL 어댑터가 그걸 indev 의 driver_data 에
 * 넣어둔다. 구조체 앞머리(매직 + 핸들)만 빌려 쓴다 — 매직으로 확인하니
 * 어댑터가 바뀌면 조용히 틀리는 게 아니라 그냥 0을 돌려준다.
 *
 * read_data 는 부르지 않는다. 어댑터가 매 주기 이미 읽어놨으므로
 * 캐시된 좌표만 꺼내면 된다 — I2C 를 두 번 때리지 않는다. */
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
        if (!tp) ESP_LOGW("touch", "터치 핸들을 못 찾았다 — 두 손가락 제스처는 꺼진다");
    }
    if (!tp) return 1;

    uint16_t x[2], y[2];
    uint8_t n = 0;
    esp_lcd_touch_get_coordinates(tp, x, y, NULL, &n, 2);
    return n;
}

/* 화면 끄기 = 패널을 진짜로 끈다.
 *
 * 예전엔 밝기만 0 으로 낮췄다(BSP 의 backlight_off 가 그것뿐이다). 화소는
 * 안 빛나지만 드라이버·게이트 스캔·부스트가 계속 돌아서, 실측으로 화면을
 * 껐는데도 켠 것의 44% 가 흘렀다(0906: 화면끔 76mV/h, 화면켬 171mV/h).
 * 이제 0x28(Display Off)로 스캔을 멈춘다. 얼마나 줄었는지는 같은 방식으로
 * 재보면 그 자리에서 나온다.
 *
 * 밝기 복원은 그대로 우리가 한다 — BSP 의 backlight_on 은 무조건 100% 라
 * 껐다 켤 때마다 사용자 설정이 날아갔다. */
/* ── 터치 칩 재우기 ───────────────────────────────────────────
 * CST9217 은 화면이 꺼져 있어도 계속 스캔한다. 전원은 LCD 와 VCC3V3 를
 * 공유해서 끊을 수 없지만, 명령으로는 재울 수 있다.
 *
 * 근거(전부 로컬 자료): SensorLib 의 CST9xxConstants.h 가
 * CST9217_CHIP_ID=0x9217 과 나란히 CST92XX_REG_SLEEP_MODE=0xD105 를 정의하고,
 * TouchDrvCST92xx/CST226 의 sleep() 이 {0xD1,0x05} 2바이트를 보낸다.
 * Hynitron 이식 매뉴얼 p.20 에도 "0xD105 Deep sleep" 으로 나온다.
 * 우리 드라이버가 쓰는 레지스터(0xD000/0xD101/0xD1FC/0xD1F8)가 같은 계열
 * 표와 1:1 로 맞는다 — CST816 자용 계열의 0xA5 명령과는 다르다.
 *
 * 깨우기는 RST(GPIO2) 를 토글한다. 슬립 중엔 I2C 가 안 먹을 수 있어서
 * 명령으로 깨우지 않는 게 정석이다. RST 가 LCD(GPIO1) 와 독립이라
 * 화면과 무관하게 터치만 되살릴 수 있다. */
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
            ESP_LOGI("tp", "터치 칩 재움 (0xD1 0x05)");
        }
    } else {
        gpio_set_direction(TP_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(TP_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TP_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(50));             /* 데이터시트 권장 여유 */
        s_tp_asleep = false;
        ESP_LOGI("tp", "터치 칩 깨움 (RST 토글)");
    }
}

void port_display_power(bool on)
{
    badge_display_on(on);
    /* 터치로 화면을 깨우지 않는 구조라(주머니 오작동 방지) 꺼진 동안
     * 터치 칩이 스캔할 이유가 없다. */
    tp_sleep(!on);
}

/* ── PWR 버튼 ─────────────────────────────────────────────────
 * BOOT 와 달리 GPIO 가 아니다. AXP2101 이 눌림을 잡아서 인터럽트 상태
 * 레지스터에 표시해두면 우리가 I2C 로 읽어간다. 그래서 아주 살짝 늦다.
 * 길게 누르면 칩이 하드웨어로 전원을 끊어버린다 — 우리가 막을 수 없다. */
#include "driver/i2c_master.h"

#define AXP_ADDR        0x34
#define AXP_REG_INTEN2  0x41
#define AXP_REG_INTSTS2 0x49
#define AXP_PKEY_LONG   0x04     /* 통합 IRQ 비트 10 */
#define AXP_PKEY_SHORT  0x08     /* 통합 IRQ 비트 11 */

static i2c_master_dev_handle_t s_axp;

static bool axp_rd(uint8_t reg, uint8_t *v)
{
    if (!s_axp) axp_init();          /* 버튼 태스크보다 먼저 부를 수도 있다 */
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
        ESP_LOGW("axp", "PMU 를 못 잡았다 — PWR 버튼은 꺼진다");
        return;
    }
    /* ── 안 쓰는 전원 레일 정리 ─────────────────────────────
     * 회로도로 확인한 결과 이 보드가 실제로 쓰는 건 둘뿐이다:
     *   DCDC1 → VCC3V3 (ESP32-S3 · LCD/터치 FPC · 코덱 디지털 · IMU · 앰프)
     *   ALDO1 → A3V3   (ES8311/ES7210 아날로그 · 마이크 바이어스)
     * 나머지 12개는 아무 데도 안 붙어 있다. DCDC2~5 는 인덕터조차 없다.
     *
     * 그런데 우리는 이 레지스터를 한 번도 안 건드렸다 — AXP 가 부팅
     * 기본값(EFUSE) 그대로 돈다. 데이터시트 기본값엔 DCDC2·3·4 와 ALDO3 가
     * 켜진 채로 올라오는 배치가 있고, 인덕터 없는 벅이 켜져 있으면 FB 가
     * 안 올라와 최대듀티나 hiccup 으로 계속 스위칭한다.
     *
     * 먼저 현재 상태를 찍고, 웨이브셰어 자기네 예제가 하는 그대로 정리한다. */
    uint8_t d0 = 0, l0 = 0, l1 = 0, v1 = 0, va = 0;
    axp_rd(0x80, &d0); axp_rd(0x90, &l0); axp_rd(0x91, &l1);
    axp_rd(0x82, &v1); axp_rd(0x92, &va);
    ESP_LOGI("axp", "정리 전  DCDC=0x%02X LDO=0x%02X/0x%02X  DCDC1=%dmV ALDO1=%dmV",
             d0, l0, l1, 500 + v1 * 10, 500 + va * 100);

    axp_wr(0x80, (uint8_t)((d0 & ~0x1E) | 0x01));   /* DCDC1 만 남긴다 */
    axp_wr(0x90, 0x01);                              /* ALDO1 만 남긴다 */
    axp_wr(0x91, (uint8_t)(l1 & ~0x01));             /* DLDO2 끔 */

    axp_rd(0x80, &d0); axp_rd(0x90, &l0); axp_rd(0x91, &l1);
    ESP_LOGI("axp", "정리 후  DCDC=0x%02X LDO=0x%02X/0x%02X", d0, l0, l1);

    /* ── 충전 설정 확인 ─────────────────────────────────────
     * 우리는 충전을 전혀 제어하지 않고 칩 기본값에 맡긴다. 그 기본값이
     * 이 배터리에 맞는지 알아야 해서 부팅 때 한 번 찍는다. */
    {
        uint8_t ipre = 0, icc = 0, iterm = 0, cv = 0, voff = 0, vsys = 0, chg = 0;
        axp_rd(0x61, &ipre); axp_rd(0x62, &icc); axp_rd(0x63, &iterm);
        axp_rd(0x64, &cv);   axp_rd(0x24, &voff); axp_rd(0x14, &vsys);
        axp_rd(0x01, &chg);
        /* 충전 전류: 0~8 은 25mA 단위, 9 부터 300/400/500... */
        int cc = (icc & 0x1F);
        int cc_ma = (cc <= 8) ? cc * 25 : 300 + (cc - 9) * 100;
        static const char *CV[] = { "-", "4.00V", "4.10V", "4.20V", "4.35V", "4.40V", "?", "?" };
        static const char *ST[] = { "대기", "예비충전", "정전류", "정전압", "완료", "멈춤", "?", "?" };
        ESP_LOGI("axp", "충전  전류 %dmA · 종료 %dmA · 만충 %s · 예비 %dmA",
                 cc_ma, (iterm & 0x0F) * 25, CV[cv & 0x07], (ipre & 0x0F) * 25);
        ESP_LOGI("axp", "보호  차단전압 %.1fV · 시스템최저 %.2fV · 지금 %s",
                 2.6 + (voff & 0x07) * 0.1, 4.1 + (vsys & 0x07) * 0.1,
                 ST[(chg >> 5) & 0x07]);
    }

    /* ── 충전 안전 교정 ─────────────────────────────────────
     * 읽어보니 기본값이 이랬다:
     *   종료 전류 125mA — 충전 전류(200mA)의 62%. 이러면 60%쯤에서 "다 찼다"고
     *     끊는다. 보통 충전 전류의 5~10% 로 잡아야 실제로 만충된다 → 25mA
     *   예비 충전 125mA — 바닥난 리튬 셀을 깨울 땐 살살 해야 한다 → 25mA
     *   차단 전압 2.6V  — 리튬은 3.0V 아래로 가면 회복 안 되는 손상이 시작된다.
     *     여기가 제일 나빴다 → 3.0V
     * 충전 전류(200mA)는 그대로 둔다. 배터리 용량을 모르는 상태에서 올리는 건
     * 위험하고, 200mA 는 이 크기 셀에 무리한 값이 아니다. */
    {
        uint8_t t = 0, ip = 0, vo = 0;
        axp_rd(0x63, &t);  axp_wr(0x63, (uint8_t)((t & 0xF0) | 0x01));   /* 종료 25mA */
        axp_rd(0x61, &ip); axp_wr(0x61, (uint8_t)((ip & 0xF0) | 0x01));  /* 예비 25mA */
        axp_rd(0x24, &vo); axp_wr(0x24, (uint8_t)((vo & 0xF8) | 0x04));  /* 차단 3.0V */
        axp_rd(0x63, &t);  axp_rd(0x61, &ip); axp_rd(0x24, &vo);
        ESP_LOGI("axp", "교정 후  종료 %dmA · 예비 %dmA · 차단 %.1fV",
                 (t & 0x0F) * 25, (ip & 0x0F) * 25, 2.6 + (vo & 0x07) * 0.1);
    }

    uint8_t v = 0;
    axp_rd(AXP_REG_INTEN2, &v);
    axp_wr(AXP_REG_INTEN2, v | AXP_PKEY_SHORT | AXP_PKEY_LONG);
    axp_wr(AXP_REG_INTSTS2, AXP_PKEY_SHORT | AXP_PKEY_LONG);   /* 묵은 것 지우기 */
}

int port_pwr_key(void)
{
    static int64_t last_us;

    uint8_t st = 0;
    if (!axp_rd(AXP_REG_INTSTS2, &st)) return 0;
    uint8_t hit = st & (AXP_PKEY_SHORT | AXP_PKEY_LONG);
    if (!hit) return 0;
    axp_wr(AXP_REG_INTSTS2, st);             /* 읽은 비트를 통째로 지운다 */

    /* 한 번 누른 게 두 번으로 세어지면 켜자마자 꺼진다. 0.4초 안엔 한 번만. */
    int64_t now = esp_timer_get_time();
    if (now - last_us < 400000) return 0;
    last_us = now;

    return (hit & AXP_PKEY_SHORT) ? 1 : 2;
}

/* ── PCF85063 RTC ─────────────────────────────────────────────
 * ESP 내부 시계는 전원이 끊기면 사라진다. 보드에 달린 RTC 칩은 배터리로
 * 계속 돌기 때문에, 부팅할 때 여기서 읽어오고 시각을 맞출 때 여기에 쓴다.
 * 안 그러면 껐다 켤 때마다 1970년이다. */
#define RTC_ADDR      0x51
#define RTC_REG_SEC   0x04       /* 초·분·시·일·요일·월·년 7바이트 (BCD) */

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
    gmtime_r(&now, &tm);                 /* 칩에는 UTC 로 넣는다 */
    uint8_t b[8] = {
        RTC_REG_SEC,
        dec2bcd(tm.tm_sec) & 0x7F,       /* bit7 = 발진 정지 표시. 0 으로 지운다 */
        dec2bcd(tm.tm_min),
        dec2bcd(tm.tm_hour),
        dec2bcd(tm.tm_mday),
        dec2bcd(tm.tm_wday),
        dec2bcd(tm.tm_mon + 1),
        dec2bcd(tm.tm_year % 100),
    };
    i2c_master_transmit(s_rtc, b, sizeof(b), 200);
    ESP_LOGI("rtc", "RTC 에 기록했다");
}

/* 시간대는 UTC 기준 분 단위 오프셋으로 들고 있다가 POSIX 문자열로 만든다.
 * POSIX 는 부호가 반대다 — 서울(UTC+9)은 "<+09>-9". */
static int s_tz_min = 9 * 60;

void port_set_tz_offset(int minutes)
{
    char tz[32];
    int m = -minutes;                       /* 부호 뒤집기 */
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

    /* 어느 주소에 뭐가 붙어 있는지 한 번 훑어본다 — 주소를 잘못 알고 있으면
     * 아무 로그 없이 조용히 실패한다. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus) {
        char found[96] = {0};
        int n = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 50) == ESP_OK && n < 80) {
                n += snprintf(found + n, sizeof(found) - n, "%02X ", a);
            }
        }
        ESP_LOGI("i2c", "장치: %s", found);
    }

    if (!rtc_open()) { ESP_LOGW("rtc", "I2C 장치 등록 실패"); return; }
    uint8_t reg = RTC_REG_SEC, d[7] = {0};
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, d, sizeof(d), 200) != ESP_OK) {
        ESP_LOGW("rtc", "RTC 가 응답하지 않는다 (주소 0x%02X)", RTC_ADDR);
        return;
    }

    if (d[0] & 0x80) {                   /* 발진이 멈춘 적 있음 = 값 못 믿는다 */
        ESP_LOGW("rtc", "RTC 가 비어 있다 — 시각 맞추기 필요");
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
    if (tm.tm_year < 120) return;        /* 2020 년보다 이르면 쓰레기 */

    /* newlib 에 timegm 이 없다. TZ 를 잠깐 UTC 로 돌려 mktime 을 쓴다. */
    setenv("TZ", "UTC0", 1); tzset();
    time_t t = mktime(&tm);
    setenv("TZ", "KST-9", 1); tzset();
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    ESP_LOGI("rtc", "RTC 에서 시각 복원: %04d-%02d-%02d %02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* ── 기울기 ───────────────────────────────────────────────────
 * QMI8658 로 중력 방향을 읽는다. 화면 평면(X·Y)에 실린 중력 성분의 각도가
 * 곧 "배지가 얼마나 돌아가 있나"다. 눕혀 놓으면 그 성분이 사라져서
 * 각도가 잡음이 된다 — 그럴 땐 못 믿는다고 알려준다. */
#include "qmi8658.h"

static qmi8658_dev_t s_imu;
static bool          s_imu_ok;
static bool          s_imu_tried;
/* 🚨 가속도계를 한 번 켜면 125Hz 로 계속 돈다. 구슬게임을 한 판 하거나
 * 자동회전을 잠깐 켜면 그 뒤로 영원히 켜져 있었다 — 화면을 꺼도 그렇다.
 * 마지막으로 읽은 지 5초가 지나면 재운다. 다음에 읽을 때 알아서 깬다.
 * 깨우는 데 몇 ms 밖에 안 걸려서 게임 조작감엔 영향이 없다. */
static bool     s_imu_awake;
static int64_t  s_imu_last_us;
/* 🚨 켠 직후 몇 ms 는 유효한 값이 안 나온다(125Hz 라 한 표본이 8ms).
 * 그 사이 값을 읽으면 직전 것이거나 0 인데, 게임과 물과 에어마우스가
 * 하나같이 "지금 이 자세가 수평" 을 그 첫 값으로 잡는다. 그러면 기준이
 * 엉뚱한 데 박혀 판이 끝에 붙고 안 움직인다 — 브릭 첫판이 안 먹고 한 번
 * 죽어야 되던 것의 정체다(0909 지적). 깬 직후엔 "아직 모른다" 고 답한다. */
static int64_t  s_imu_wake_us;
#define IMU_SETTLE_US  30000

static void imu_touch(void)      /* 방금 썼다고 표시 */
{
    s_imu_last_us = esp_timer_get_time();
    if (s_imu_ok && !s_imu_awake) {
        qmi8658_enable_accel(&s_imu, true);
        s_imu_awake = true;
        s_imu_wake_us = s_imu_last_us;
    }
}

/* 깬 지 얼마 안 됐으면 아직 못 믿는다 */
static bool imu_settled(void)
{
    return s_imu_wake_us == 0 ||
           esp_timer_get_time() - s_imu_wake_us >= IMU_SETTLE_US;
}

void port_imu_idle_check(void)   /* 런처가 주기적으로 부른다 */
{
    if (!s_imu_ok || !s_imu_awake) return;
    if (esp_timer_get_time() - s_imu_last_us < 5000000LL) return;
    qmi8658_enable_accel(&s_imu, false);
    s_imu_awake = false;
    ESP_LOGI("imu", "안 쓴 지 5초 — 가속도계 재움");
}

static float s_ax, s_ay, s_az;

/* 화면 법선(Z)보다 평면 성분이 확실히 커야 "세워 들었다"고 본다.
 * 책상에 눕히면 Z 가 크게 나오고, 그때 각도는 의미가 없다. */
bool port_imu_upright(void)
{
    float plane = sqrtf(s_ax * s_ax + s_ay * s_ay);
    return plane > 700.0f && plane > fabsf(s_az);
}

/* 세 축을 그대로 준다. 단위는 mg (1g ≈ 1000).
 * 🚨 x,y 만 쓰면 '기울였나' 밖에 모른다. 흔들면 세기가 변하는데 그건
 * 세 축의 크기에 들어 있다 — 물처럼 흔들림에 반응해야 하는 것엔 이게 필요하다. */
bool port_imu_accel3(float *x, float *y, float *z)
{
    float d;
    port_imu_angle(&d);              /* 값을 새로 읽어 두게 한다 */
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

/* 🚨 초기화를 여기로 뺐다. 예전엔 port_imu_angle 안에 있어서 자이로만 쓰는
 * 쪽(에어마우스)은 칩을 깨울 길이 없었다. */
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
            /* 🚨 켠 직후는 재웠다 깬 것과 똑같이 못 믿는다. 여기서 안 잡아
             * 두면 부팅 뒤 첫 읽기가 x=y=z=4000(있을 수 없는 값)인데도
             * "믿을 만함" 으로 나간다(0909 벤치에서 잡음). */
            s_imu_wake_us = esp_timer_get_time();
            ESP_LOGI("imu", "QMI8658 붙었다");
        } else {
            ESP_LOGW("imu", "QMI8658 초기화 실패 — 기울기 보정 꺼짐");
        }
    }
    return s_imu_ok;
}

/* ── 자이로 ────────────────────────────────────────────────────
 * 🔋 켜면 계속 돈다. 켠 쪽이 끄기 전엔 안 꺼진다 — 가속도계처럼 "안 쓰면
 * 재우기" 를 안 붙인 이유는, 에어마우스는 몇 초씩 가만히 겨누고 있는 것이
 * 정상이라 그걸 무동작으로 보면 안 되기 때문이다. */
static bool    s_gyro_on;
static int64_t s_gyro_wake_us;
/* 🚨 40ms 로는 모자랐다. 자이로는 켜고 나서 설익은 값을 한동안 뱉는데,
 * 그걸 영점으로 잡으면 커서가 그만큼 영영 흐른다(0910 실기). */
#define GYRO_SETTLE_US 120000

void port_imu_gyro_enable(bool on)
{
    if (!imu_ready()) return;
    if (on == s_gyro_on) return;
    if (on) {
        /* 512dps 면 손목을 세게 털어도 안 잘린다(사람 손목이 대략 500dps).
         * ODR 은 보내는 주기(초당 66회)보다 넉넉하면 된다. */
        qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_512DPS);
        qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_250HZ);
        /* 🚨 단위를 여기서 못 박는다. 초기화가 dps 로 잡아 주긴 하지만,
         * 이 값 하나가 라디안으로 뒤집히면 커서가 57배 굼떠지고 원인을 찾기
         * 어렵다 — 각도로 읽는다는 걸 코드에 남겨둔다. */
        s_imu.gyro_unit_rads = false;
        qmi8658_enable_gyro(&s_imu, true);
        s_gyro_wake_us = esp_timer_get_time();
    } else {
        qmi8658_enable_gyro(&s_imu, false);
    }
    s_gyro_on = on;
    ESP_LOGI("imu", "자이로 %s", on ? "켬" : "끔");
}

bool port_imu_gyro(float *x, float *y, float *z)
{
    /* 🚨 켠 직후는 못 믿는다 — 가속도계에서 첫 값이 있을 수 없는 수로 나와
     * 기준이 엉뚱하게 박히던 것과 같은 함정이다. */
    if (!s_gyro_on) return false;
    if (esp_timer_get_time() - s_gyro_wake_us < GYRO_SETTLE_US) return false;

    /* 🚨 헤더에 qmi8658_read_gyro_dps() 가 선언돼 있는데 **구현이 없다**
     * (벤더 드라이버가 선언만 해뒀다 — 링크에서야 안다). 실제로 있는 건
     * qmi8658_read_gyro() 뿐이고, 그게 gyro_unit_rads 를 보고 단위를 고른다. */
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

    /* 드라이버가 g 가 아니라 mg 로 준다 (1g ≈ 1000). 실기 로그로 확인했다.
     * 화면 평면에 실린 성분이 0.35g 미만이면 눕혀 둔 것 — 각도가 잡음이다. */
    s_ax = x; s_ay = y; s_az = z;

    float mag = sqrtf(x * x + y * y);
    if (mag < 350.0f) return false;

    *deg = atan2f(x, y) * 57.29578f;

    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last > 2000000) {
        last = now;
        ESP_LOGI("imu", "accel x=%.2f y=%.2f z=%.2f → %.0f도", x, y, z, *deg);
    }
    return true;
}

/* ── 배터리 ───────────────────────────────────────────────────
 * AXP2101 이 잔량을 계산해서 레지스터 하나에 넣어준다. 우리가 전압으로
 * 어림잡을 필요가 없다. 상태2(0x01) 비트 3 이 배터리 있음, 비트 5~6 이 충전중. */
#define AXP_REG_STATUS1   0x00      /* 비트3 = 배터리 있음 (벤더 코드 확인) */
#define AXP_REG_STATUS2   0x01      /* 비트[6:5] = 00 대기 / 01 충전 / 10 방전 */
#define AXP_REG_BAT_PCT   0xA4

/* AXP2101 공통설정(0x10) 비트0 = 소프트 종료.
 * 전에는 "power off" 글자만 띄우고 실제로는 계속 켜져 있었다 — 그게 더 나쁘다.
 * 이제 진짜로 끊는다. 다시 켜려면 PWR 을 다시 눌러야 한다. */
#define AXP_REG_COMMON  0x10

void port_power_off(void)
{
    uint8_t v = 0;
    if (!axp_rd(AXP_REG_COMMON, &v)) return;
    ESP_LOGI("axp", "전원 차단");
    axp_wr(AXP_REG_COMMON, v | 0x01);
}

int port_battery_percent(void)
{
    uint8_t st1 = 0, pct = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st1)) {
        ESP_LOGW("batt", "PMU 를 못 읽는다");
        return -1;
    }
    if (!(st1 & 0x08)) {
        ESP_LOGD("batt", "배터리 없음 (STATUS1=0x%02X)", st1);
        return -1;
    }
    if (!axp_rd(AXP_REG_BAT_PCT, &pct)) return -1;

    static bool logged;
    if (!logged) { logged = true; ESP_LOGI("batt", "STATUS1=0x%02X 잔량=%d%%", st1, pct); }
    int p = pct > 100 ? 100 : pct;
    batt_track(p, (st1 & 0x20) != 0);
    return p;
}

/* ── 남은 시간 ────────────────────────────────────────────────
 * 용량(mAh)도 전류계도 없다. 그래서 "1%가 떨어지는 데 걸린 시간"을 직접 재서
 * 남은 %에 곱한다 — 지어낸 숫자가 아니라 이 기기에서 실제로 관측한 값이다.
 * 화면을 켜두면 빨리, 꺼두면 천천히 닳으므로 값은 계속 움직인다.
 * 그래서 표시도 "지금 속도로"라고 못박는다. */
static int      s_last_pct = -1;
static int64_t  s_last_drop_us;
static float    s_min_per_pct;      /* 1% 당 분. 0 이면 아직 모름 */

/* 🚨 재는 방식은 맞다 — 전류계가 없으니 "1%가 떨어지는 데 몇 분" 을 직접
 * 잰다. 틀린 건 그 값을 하나만 들고 있다는 것이다. 방전 곡선은 직선이
 * 아니라서 잔량에 따라 속도가 다르다. 이 배지가 스스로 남긴 일지가 그렇게
 * 말한다(0909, 186줄):
 *     위쪽 100→63% : 33,544초에 37% = 15.1분/%
 *     아래쪽 62→ 3% : 18,180초에 59% =  5.1분/%   ← 세 배 차이
 * 게다가 EMA(0.7:0.3)가 충전 경계를 넘어 이어지니, 지난 방전의 바닥값이
 * 이번 방전의 꼭대기까지 따라온다. 실제로 최근 실측은 16.4분/% 인데 학습값은
 * 11.0분/% 였고, 설정 화면은 86%에서 "15h 46m" 을 띄웠다 — 일지가 말하는
 * 23시간보다 30% 넘게 짧다.
 * 그래서 구간을 나눠 따로 배우고, 남은 시간은 구간을 이어 더한다. */
#define BAND_N 3
static const uint8_t BAND_LO[BAND_N] = { 60, 30, 0 };   /* 각 구간의 아래 끝 */
static float s_mpp[BAND_N];                              /* 구간별 1%당 분 */
/* 🚨 그 칸을 실제로 배웠나. 옛 단일 값을 세 칸에 복사한 '씨앗' 과 진짜로
 * 잰 값을 갈라야 한다 — 0911 에 30~59% 와 0~29% 가 나란히 11.0분/% 으로
 * 찍혀 있었는데 둘 다 안 재본 씨앗이었다. 같은 일지에 끝까지 방전한 기록이
 * 있어 손으로 재보니 7.9분/% 였다. **안 재본 값이 잰 값처럼 보이면 안 된다.**
 * 별도 키에 둔다 — mppb 형식을 건드리면 이미 배운 60~100% 가 날아간다. */
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
    /* 🚨 전체 하나짜리 값도 여기서 되살린다. 예전엔 NVS 에 **쓰기만 하고 읽는
     * 데가 없었다** — 그래서 켤 때마다 0 에서 시작했고, 부팅 뒤 첫 1% 가
     * 떨어지는 순간 그동안 쌓은 평균을 통째로 갈아치웠다(`== 0` 가지).
     * 하루에 열두 번 꽂았다 뽑는 물건이라 사실상 "방금 잰 값 하나" 였고,
     * 그래서 0909 에 23.1시간, 0910 에 38.6시간처럼 크게 튀었다. */
    uint32_t one = 0;
    if (nvs_get_u32(nh, "mpp", &one) == ESP_OK && one) s_min_per_pct = one / 100.0f;
    size_t nlen = sizeof(s_mpp_seen);
    if (nvs_get_blob(nh, "mppn", s_mpp_seen, &nlen) != ESP_OK || nlen != sizeof(s_mpp_seen))
        memset(s_mpp_seen, 0, sizeof(s_mpp_seen));
    size_t len = sizeof(s_mpp);
    if (nvs_get_blob(nh, "mppb", s_mpp, &len) != ESP_OK || len != sizeof(s_mpp)) {
        /* 구간별 값이 아직 없다. 예전에 하나로 배운 값이 있으면 세 칸에 그대로
         * 넣어 둔다 — 갈아 끼우자마자 "재는 중" 으로 돌아가면 안 되니까.
         * 다음 방전부터 구간별로 갈린다. */
        uint32_t v = 0;
        if (nvs_get_u32(nh, "mpp", &v) == ESP_OK && v)
            for (int b = 0; b < BAND_N; b++) s_mpp[b] = v / 100.0f;
    }
    nvs_close(nh);
}

/* 그 구간을 아직 못 배웠으면 가장 가까운 아는 구간의 값을 빌려 쓴다. */
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
    if (plugged) {                  /* 충전 중엔 측정을 접는다 */
        s_last_pct = -1;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_pct < 0) { s_last_pct = pct; s_last_drop_us = now; return; }

    /* 🚨 잔량이 도로 올랐다 = 아까 낮게 읽힌 건 진짜로 쓴 게 아니라 부하로
     * 전압이 눌렸던 것이다(AXP2101 잔량계는 전압을 보고 계산한다. 쿨롱
     * 카운터가 아니다). 0909 실기: 녹음 30분 뒤 83% → 녹음 끝나자 88%.
     *
     * 예전엔 오르면 그냥 넘어갔는데, 그러면 기준점이 눌린 값에 머물러서
     * 같은 구간을 두 번 센다:
     *     90 → (눌림) 88 : 2% 썼다고 기록
     *        → (회복) 90 : 무시  ← 여기서 기준을 안 올린 게 잘못
     *        → (진짜) 88 : 또 2% 썼다고 기록
     * 그래서 학습된 소모율이 실제보다 빠르게 나왔다. 오르면 기준을 새로 잡는다. */
    if (pct > s_last_pct) {
        ESP_LOGI("batt", "잔량이 %d%%→%d%% 로 회복 — 부하로 눌렸던 것이다. 기준 다시 잡음",
                 s_last_pct, pct);
        s_last_pct = pct;
        s_last_drop_us = now;
        return;
    }
    if (pct == s_last_pct) return;  /* 아직 안 떨어졌다 */

    int drop = s_last_pct - pct;
    float mins = (float)(now - s_last_drop_us) / 60000000.0f / drop;
    s_last_pct = pct;
    s_last_drop_us = now;
    if (mins < 0.2f || mins > 600.0f) return;      /* 말도 안 되는 값은 버린다 */

    /* 처음 한 번은 그대로, 그 뒤로는 천천히 섞는다.
     * 🚨 섞기 전에 저장분을 먼저 되살려야 한다 — 안 그러면 부팅 첫 방울이
     *    쌓아둔 값을 갈아치운다. mpp_load 는 두 번 불러도 공짜다. */
    mpp_load();
    s_min_per_pct = (s_min_per_pct == 0) ? mins : s_min_per_pct * 0.7f + mins * 0.3f;

    /* 떨어져 들어간 자리의 구간에 넣는다. 완만한 위쪽과 가파른 아래쪽이
     * 서로를 오염시키지 않게. (위에서 이미 불러왔다) */
    int b = band_of(pct);
    /* 🚨 씨앗 위에 처음 얹을 땐 섞지 말고 갈아치운다. 안 그러면 안 재본
     * 값이 30% 씩만 밀려나 한참 동안 거짓말이 남는다. */
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

    /* 🚨 잔량에 값 하나를 곱하지 않는다. 1%씩 바닥까지 내려가면서 그 자리
     * 구간의 값을 더한다 — 아래쪽이 가파른 만큼 실제로 짧게 계산된다. */
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

/* 100% 가 되면 충전이 끝나서 "충전 중"이 아니게 된다. 그래도 꽂혀는 있으니
 * 표시는 남아야 한다 — STATUS1 비트5 가 VBUS 있음이다. */
bool port_battery_plugged(void)
{
    uint8_t st = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st)) return false;
    return (st & 0x20) != 0;
}




/* ── 배터리 실측 로거 ──────────────────────────────────────
 * AXP2101 은 전류계를 안 내주고 전압만 준다(0x34/0x35, 상위 5비트+하위 8비트 = mV).
 * 그래서 "몇 mA 먹나"를 칩에 물어볼 수가 없다. 대신 쓰면서 전압과 퍼센트를
 * 1분마다 남겨두면 실제 소모 곡선이 저절로 쌓인다. 추정을 실측으로 바꾸는 값싼 방법. */
#define AXP_REG_ADC_H 0x34
#define AXP_REG_ADC_L 0x35

int port_battery_mv(void)
{
    uint8_t h, l;
    if (!axp_rd(AXP_REG_ADC_H, &h) || !axp_rd(AXP_REG_ADC_L, &l)) return -1;
    int mv = ((h & 0x1F) << 8) | l;
    return mv > 0 ? mv : -1;
}

/* ── 배터리 일지 ──────────────────────────────────────────────
 * 케이블을 뽑으면 시리얼로 나가는 로그를 받아 적을 데가 없다. 그래서
 * 배지가 스스로 NVS 에 남긴다. 간격은 아래에서 1분 → 5분 → 15분으로
 * 늘려서 200줄이 정확히 하루를 덮는다(40분 + 500분 + 900분 = 24시간).
 * 🚨 그래서 뒷부분은 15분 간격이다 — 기울기를 낼 때 줄 간격을 고정으로
 *    보면 안 된다. 충전이 시작되면 지우고 새로 쓴다 — 한 번 뽑은
 * 구간만 깨끗하게 담기게.
 *
 * 아침에 케이블 꽂으면 부팅 로그에 표로 뱉는다. */
#define JRN_MAX 200
/* 🚨 sec 가 uint16 이면 18.2시간에서 넘친다 — 하루를 못 담는다. uint32 로.
 * flags: bit0 = 화면 켜짐, bit1~5 = 그때 하던 일(port_crumb 값).
 * 이러면 평소처럼 쓰기만 해도 "어느 앱이 얼마나 먹나"가 저절로 쌓인다. */
typedef struct { uint32_t sec; uint16_t mv; uint8_t pct; uint8_t flags; } jrn_t;
#define JRN_SCR(f)   ((f) & 1)
#define JRN_CRUMB(f) (((f) >> 1) & 0x1F)
/* 이 줄 앞에서 끊겼다는 표시 — 충전했거나 재부팅했다. 이 경계를 가로질러
 * 기울기를 내면 안 된다(충전하면 전압이 도로 올라가니 값이 뒤집힌다). */
#define JRN_BREAK    0x40
static jrn_t   s_jrn[JRN_MAX];
static uint16_t s_jrn_n;
static bool     s_jrn_loaded;

/* 🚨 일지 구조가 바뀌면 NVS 에 남은 옛 데이터를 새 자리로 읽어 쓰레기가 된다
 * (0907: sec 를 uint16→uint32, scr→flags 로 바꾸고 그대로 겪었다).
 * 판 번호를 같이 저장해서 다르면 버린다. 구조를 손대면 이 숫자를 올릴 것. */
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
    if (ver != JRN_VER) { n = 0; ESP_LOGI("batt", "일지 형식이 바뀌었다 — 옛 기록 버림"); }
    if (n > JRN_MAX) n = 0;
    if (n && nvs_get_blob(nh, "jrn", s_jrn, &len) == ESP_OK) s_jrn_n = (uint16_t)n;
    nvs_close(nh);
}

/* 일지 한 줄. 예전엔 port_battery_log 안에만 있었는데, 화면을 켜고 끄는
 * 자리에서도 남겨야 해서 밖으로 뺐다. */
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

/* ── 부팅 기록 ────────────────────────────────────────────────
 * "왜 재부팅됐나"는 사유만으론 안 풀린다. 죽기 직전에 뭘 하고 있었는지,
 * 얼마나 켜져 있었는지, 배터리가 얼마였는지가 같이 있어야 좁혀진다.
 *
 * 🚨 이 기록은 어떤 경우에도 스스로 지우지 않는다. 읽으려면 케이블을 꽂아야
 * 하는데 꽂는 순간 지워지면 아무 소용이 없다(0906 에 배터리 일지를 그렇게
 * 날렸다). 지우는 건 사람이 badge-diag.sh --clear 로만 한다. */
#define BOOT_MAX 12
typedef struct {
    uint8_t  reason;    /* esp_reset_reason_t */
    uint8_t  pct;       /* 부팅 시 배터리 % */
    uint8_t  crumb;     /* 죽기 직전 하던 일 (port_crumb) */
    uint8_t  rep;       /* 같은 것이 이어서 몇 번 더 있었나 (0 = 한 번뿐) */
    uint32_t ran_sec;   /* 그 전에 몇 초나 켜져 있었나 */
} boot_rec_t;

static const char *CRUMB_NAME[] = {
    "부팅직후", "홈", "잠금화면", "화면끔", "화면켬",
    "앱열기", "마우스", "회의", "게임", "계산기", "시계", "설정", "녹음중",
};
#define CRUMB_N (sizeof(CRUMB_NAME)/sizeof(CRUMB_NAME[0]))

static uint8_t s_crumb;

/* 지금 뭘 하는지 남긴다. 전환 때만 부르므로 플래시 마모는 무시할 수준. */
int port_crumb_now(void) { return (int)s_crumb; }

void port_crumb(int what)
{
    if (what < 0 || what >= (int)CRUMB_N || what == s_crumb) return;

    /* 🚨 앱이 바뀌는 자리에 일지를 두 줄 남긴다 — 옛 앱 이름으로 하나(그 앱의
     * 구간을 닫는다), 새 앱 이름으로 하나(다음 구간을 연다). 한 줄만 남기면
     * 둘 중 한 앱의 구간이 통째로 사라진다. 두 줄의 시각이 같아 그 사이는
     * 기울기 계산에서 저절로 빠진다(t <= 0 을 버린다).
     * 🚨 화면이 켜져 있고 뽑혀 있을 때만. 꺼져 있으면 앱이 없고, 꽂혀 있으면
     * 방전이 아니다. 앱을 빨리 오가도 일지가 안 넘치게 텀을 둔다. */
    if (!port_battery_plugged() && !launcher_screen_is_off()) {
        static int64_t last_swap;
        int64_t now = esp_timer_get_time();
        if (!last_swap || now - last_swap >= 10000000LL) {
            int mv = port_battery_mv(), pct = port_battery_percent();
            if (mv > 0 && pct >= 0) {
                jrn_load();
                last_swap = now;
                jrn_put(now, mv, pct, true, false);          /* 옛 앱으로 닫는다 */
                s_crumb = (uint8_t)what;
                jrn_put(now, mv, pct, true, false);          /* 새 앱으로 연다 */
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

/* 켜져 있는 동안 60초마다 불린다. 다음 부팅 때 "얼마나 살아 있었나"가 된다. */
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
        "알수없음","전원켜짐","외부리셋","소프트리셋","패닉","인터럽트워치독",
        "태스크워치독","기타워치독","딥슬립복귀","브라운아웃","SDIO",
        "USB리셋","JTAG리셋","eFuse오류","전원글리치","CPU잠김",
    };
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) != ESP_OK) return;

    /* 직전 실행이 남긴 것 */
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
    /* 🚨 진단하러 케이블을 꽂는 것 자체가 USB 리셋이다. 60초를 못 채우면
     * port_uptime_mark 가 한 번도 안 돌아 '직전 0초' 로 남는다. 12칸뿐인
     * 기록에 이런 줄이 절반을 차지했다(0909: 12건 중 5건) — 읽으러 꽂을
     * 때마다 진짜 기록이 한 칸씩 밀려난 것이다. 기록을 스스로 지우지
     * 않는다는 원칙을 세워놓고, 정작 읽는 행위가 기록을 갉아먹고 있었다.
     * 같은 것이 이어지면 새 칸을 쓰지 말고 세기만 한다. 패닉·워치독·
     * 브라운아웃은 사유가 달라 여기 안 걸린다 — 뭉쳐도 안전하다. */
    if (n && rr == ESP_RST_USB && ran == 0
        && ring[n - 1].reason == (uint8_t)rr && ring[n - 1].ran_sec == 0) {
        if (ring[n - 1].rep < 255) ring[n - 1].rep++;
        ring[n - 1].pct = r.pct;
    } else {
        if (n >= BOOT_MAX) {             /* 가장 오래된 걸 밀어낸다 */
            memmove(&ring[0], &ring[1], sizeof(boot_rec_t) * (BOOT_MAX - 1));
            n = BOOT_MAX - 1;
        }
        ring[n++] = r;
    }
    nvs_set_blob(nh, "boot", ring, sizeof(boot_rec_t) * n);
    nvs_set_u32(nh, "bootn", n);
    nvs_set_u32(nh, "up", 0);            /* 새 구간 시작 */
    nvs_commit(nh);
    nvs_close(nh);

    ESP_LOGI("rst", "─── 부팅 기록 %lu개 (최근이 아래) ───", (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        const char *rn = ring[i].reason < 16 ? RNAME[ring[i].reason] : "?";
        const char *cn = ring[i].crumb < CRUMB_N ? CRUMB_NAME[ring[i].crumb] : "?";
        char rep[28] = "";
        if (ring[i].rep)
            snprintf(rep, sizeof(rep), "  x%u (꽂은 것)", (unsigned)ring[i].rep + 1);
        ESP_LOGI("rst", "  %2lu) %-10s  직전 %5lu초 켜짐  배터리 %3u%%  하던일=%s%s",
                 (unsigned long)i + 1, rn, (unsigned long)ring[i].ran_sec,
                 ring[i].pct, cn, rep);
    }
    ESP_LOGI("rst", "  ※ 패닉/워치독이 '화면켬'에서 반복되면 깨우는 경로가 범인이다");
}

void port_battery_journal_dump(void)
{
    /* 일지가 날아가도 이건 남는다 — 실제로 관측한 "1% 당 몇 분"이다.
     * 방전 중 1%가 떨어질 때마다 갱신되어 NVS 에 쌓인 값. */
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
        if (mah) ESP_LOGI("batt", "측정된 배터리 용량 약 %lumAh", (unsigned long)mah);
        if (v) {
            float mpp = v / 100.0f;
            ESP_LOGI("batt", "학습된 소모율(전체 하나): 1%%당 %.1f분 → 100%%면 %.1f시간 (시간당 %.1f%%)",
                     mpp, mpp * 100.0f / 60.0f, 60.0f / mpp);
        } else {
            ESP_LOGI("batt", "학습된 소모율 없음");
        }
        /* 🚨 구간별로 나눠 배운 값. 이게 안 갈리면 남은 시간이 양 끝에서
         * 틀린다(0909: 86%에서 15h46m 이라 띄웠는데 일지는 23시간이었다).
         * 세 값이 서로 달라지기 시작하면 제대로 배우는 중이다. */
        mpp_load();
        {
            char line[128]; int off = 0;
            for (int b = 0; b < BAND_N; b++) {
                int hi = (b == 0) ? 100 : BAND_LO[b - 1] - 1;
                off += snprintf(line + off, sizeof(line) - off, "%s%d~%d%%:",
                                b ? "  " : "", BAND_LO[b], hi);
                if (s_mpp[b] <= 0)
                    off += snprintf(line + off, sizeof(line) - off, "아직");
                else if (s_mpp_seen[b] == 0)
                    /* 안 재봤다. 옛 단일 값을 빌려 쓰는 중이라고 밝힌다. */
                    off += snprintf(line + off, sizeof(line) - off,
                                    "%.1f분/%%(빌림)", s_mpp[b]);
                else
                    off += snprintf(line + off, sizeof(line) - off,
                                    "%.1f분/%%(%d회)", s_mpp[b], s_mpp_seen[b]);
            }
            ESP_LOGI("batt", "구간별 소모율: %s", line);
        }
        {
            int left = port_battery_minutes_left();
            if (left > 0) ESP_LOGI("batt", "지금 잔량으로 남은 시간 %dh %02dm (구간을 이어 더한 값)",
                                   left / 60, left % 60);
        }
    }
    jrn_load();
    if (!s_jrn_n) { ESP_LOGI("batt", "일지 비어 있음"); return; }
    ESP_LOGI("batt", "─── 배터리 일지 %u줄 (화면 O/X) ───", s_jrn_n);
    for (uint16_t i = 0; i < s_jrn_n; i++) {
        uint8_t cb = JRN_CRUMB(s_jrn[i].flags);
        if (s_jrn[i].flags & JRN_BREAK)
            ESP_LOGI("batt", "  ──── 여기서 끊김 (충전했거나 재부팅) ────");
        ESP_LOGI("batt", "%6lu초  %4umV  %3u%%  화면%s  %s",
                 (unsigned long)s_jrn[i].sec, s_jrn[i].mv, s_jrn[i].pct,
                 JRN_SCR(s_jrn[i].flags) ? "O" : "X",
                 cb < CRUMB_N ? CRUMB_NAME[cb] : "?");
    }

    /* 화면 켠 구간과 끈 구간의 전압 기울기를 따로 낸다. 둘의 차이가
     * 곧 "화면이 먹는 몫"이다. %는 4~5분에 한 칸이라 너무 굵어서 mV 로 본다. */
    for (int mode = 0; mode < 2; mode++) {
        long dt = 0; long dv = 0; int seg = 0;
        for (uint16_t i = 1; i < s_jrn_n; i++) {
            if (s_jrn[i].flags & JRN_BREAK) continue;   /* 충전·재부팅을 가로지르면 값이 뒤집힌다 */
            if (JRN_SCR(s_jrn[i].flags) != mode || JRN_SCR(s_jrn[i-1].flags) != mode) continue;
            long t = (long)s_jrn[i].sec - (long)s_jrn[i-1].sec;
            int v = (int)s_jrn[i-1].mv - (int)s_jrn[i].mv;   /* 떨어진 양 */
            if (t <= 0 || t > 1200) continue;                /* 재부팅으로 끊긴 구간은 버린다 */
            dt += t; dv += v; seg++;
        }
        if (seg && dt > 0) {
            ESP_LOGI("batt", "── 화면%s: %ld초 동안 %ldmV 내려감 → 시간당 %.0fmV (표본 %d)",
                     mode ? "O" : "X", dt, dv, dv * 3600.0 / dt, seg);
        } else {
            ESP_LOGI("batt", "── 화면%s: 표본 부족", mode ? "O" : "X");
        }
    }
    /* 앱별 소모 — 화면 켜진 구간만, 하던 일별로 나눠 낸다.
     * "시계 앱이 잠금화면보다 몇 배 먹나" 같은 질문에 이게 답한다.
     *
     * 🚨 이 값은 앱을 **한 번에 몇 분씩** 써야 쌓인다. 잠깐씩 들락거리면
     * 구간이 전부 문턱 아래라 한 줄도 안 나온다 — 그게 맞는 동작이다.
     * 전압으로 재는 물건의 한계다(이 보드엔 쿨롱 카운터가 없다). */
    #define APP_SEG_MIN 120   /* 초. 이보다 짧은 구간은 회복 곡선이라 버린다 */
    for (unsigned c = 0; c < CRUMB_N; c++) {
        long dt = 0, dv = 0; int seg = 0;
        for (uint16_t i = 1; i < s_jrn_n; i++) {
            if (s_jrn[i].flags & JRN_BREAK) continue;   /* 충전·재부팅을 가로지르면 값이 뒤집힌다 */
            /* 🚨 예전엔 두 줄의 '하던 일' 이 **둘 다** 같아야 셌다. 그런데
             * 화면 켠 구간은 거의 늘 앱이 바뀌면서 끝난다 — 켤 땐 잠금화면,
             * 끌 땐 홈 이런 식이라 짝이 하나도 안 맞았다. 그래서 일지에
             * 화면O 가 아홉 줄이나 쌓였는데도 앱별 값이 한 줄도 안 나왔다
             * (0910 실기). 구간은 그것을 **시작한** 앱에게 준다. */
            if (JRN_CRUMB(s_jrn[i-1].flags) != c) continue;
            if (!JRN_SCR(s_jrn[i].flags) || !JRN_SCR(s_jrn[i-1].flags)) continue;
            long t = (long)s_jrn[i].sec - (long)s_jrn[i-1].sec;
            long v = (long)s_jrn[i-1].mv - (long)s_jrn[i].mv;
            /* 🚨 짧은 구간은 소모가 아니라 **전압 회복**을 잰다. 부하가 줄면
             * 배터리 내부저항 때문에 전압이 도로 올라간다 — 0911 일지에
             * 30초 만에 15mV 오른 줄이 있었다(시간당 +1800mV 짜리 헛값이다).
             * 그런 줄이 몇 개만 섞여도 합이 뒤집힌다: [홈] 이 시간당 -33mV,
             * [잠금화면] 이 게임보다 높은 600mV 로 나왔다. 검은 시계가 게임보다
             * 많이 먹을 리가 없다.
             * 눌림(sag)과 회복은 수십 초면 잦아드므로 그보다 긴 것만 센다. */
            if (t < APP_SEG_MIN || t > 1200) continue;
            dt += t; dv += v; seg++;
        }
        /* 🚨 표본이 얇으면 아예 안 찍는다. 틀린 숫자보다 "아직 모른다" 가 낫다 —
         * 사람이 그 숫자를 믿고 판단하기 때문이다. */
        if (seg >= 2 && dt >= 600)
            ESP_LOGI("batt", "── [%s] 켜진 채 %ld분 → 시간당 %.0fmV (표본 %d)",
                     CRUMB_NAME[c], dt / 60, dv * 3600.0 / dt, seg);
    }

    /* 🚨 처음부터 끝까지로 재면 충전 구간을 가로지른다(전압도 %도 도로 올라간다).
     * 마지막으로 끊긴 자리부터만 본다 — 그게 "지금 이어지는 한 판"이다. */
    uint16_t seg0 = 0;
    for (uint16_t i = s_jrn_n; i-- > 0; ) {
        if (s_jrn[i].flags & JRN_BREAK) { seg0 = i; break; }
    }
    if (s_jrn_n - seg0 >= 2) {
        long dsec = (long)s_jrn[s_jrn_n-1].sec - (long)s_jrn[seg0].sec;
        int dpct = (int)s_jrn[seg0].pct - (int)s_jrn[s_jrn_n-1].pct;
        if (dsec > 0 && dpct > 0)
            ESP_LOGI("batt", "── 마지막 구간 %ld분에 %d%% → 시간당 %.1f%%, 100%%면 %.1f시간 (%u줄 중 %u줄째부터)",
                     dsec / 60, dpct, dpct * 3600.0 / dsec, dsec * 100.0 / dpct / 3600.0,
                     s_jrn_n, (unsigned)(seg0 + 1));
    }
}

/* ── 배터리 용량 역산 ─────────────────────────────────────────
 * 웨이브셰어가 용량을 어디에도 안 적어놨다(자기네 문서 확인). 그런데 충전
 * 전류를 아니까 시간으로 역산할 수 있다:
 *     용량(mAh) ≈ 충전전류(mA) x 걸린시간(h) / 채운비율
 * 충전이 시작될 때의 %와 시각을 잡아두고, 100% 에 닿으면 계산해 NVS 에 남긴다.
 * 정전압 구간에서는 전류가 줄어드니 실제보다 조금 작게 나온다 — 하한으로 본다. */
#define CHG_MA 200

static void charge_track(int pct, bool plugged)
{
    static bool     was;
    static int      start_pct;
    static int64_t  start_us;

    if (plugged && !was) {                 /* 방금 꽂혔다 */
        was = true; start_pct = pct; start_us = esp_timer_get_time();
        ESP_LOGI("axp", "충전 시작 %d%% — 100%% 까지 재서 용량을 낸다", pct);
        return;
    }
    if (!plugged) { was = false; return; }
    if (pct < 100 || start_pct < 0 || start_pct >= 95) return;

    float hours = (esp_timer_get_time() - start_us) / 3600000000.0f;
    float filled = (100 - start_pct) / 100.0f;
    if (hours < 0.15f || filled < 0.15f) { start_pct = -1; return; }
    int mah = (int)(CHG_MA * hours / filled);
    ESP_LOGI("axp", "★ 용량 추정 %dmAh  (%d%%→100%%, %.2f시간, %dmA)",
             mah, start_pct, hours, CHG_MA);
    nvs_handle_t nh;
    if (nvs_open("badge", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u32(nh, "mah", (uint32_t)mah);
        nvs_commit(nh); nvs_close(nh);
    }
    start_pct = -1;                        /* 한 번만 */
}

void port_battery_log(const char *what)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (last && now - last < 25000000LL) return;   /* 25초 가드 — 런처가 60초마다 부른다 */
    last = now;
    int mv = port_battery_mv();
    if (mv < 0) return;
    int pct = port_battery_percent();
    bool plug = port_battery_plugged();
    charge_track(pct, plug);
    ESP_LOGI("batt", "%s t=%llds %dmV %d%% %s", what ? what : "-",
             (long long)(now / 1000000), mv, pct, plug ? "(USB)" : "");

    /* 🚨 예전엔 "충전이 시작되면 지운다"였는데, 일지를 읽으려면 케이블을 꽂아야
     * 한다. 꽂는 순간 지워져서 밤새 잰 걸 통째로 잃었다(0906). 자기모순이었다.
     * 이제는 지우지 않는다 — 다음 방전이 "시작될 때"만 새로 연다. */
    jrn_load();
    static int64_t last_jrn;
    static bool    was_plugged;
    if (plug) {
        was_plugged = true;
        return;                       /* 충전 중엔 적지도, 지우지도 않는다 */
    }
    /* 🚨 예전엔 뽑을 때마다 일지를 통째로 지웠다. 그러면 낮에 잠깐 충전하고
     * 다시 뽑는 순간 오전 기록이 다 날아간다(0909 에 그 계획을 듣고 발견).
     * 지우지 않고 "여기서 끊겼다"는 표시만 남기고 이어 쓴다. 표시를 보고
     * 기울기 계산이 그 경계를 안 넘게 한다. */
    static bool mark_break;
    if (was_plugged) {
        was_plugged = false;
        mark_break = true;
        ESP_LOGI("batt", "케이블 빠짐 — 일지에 경계만 남기고 이어 쓴다");
    }
    if (!last_jrn) mark_break = true;   /* 부팅 직후 첫 줄도 끊긴 자리다(초가 0으로 돌아간다) */

    /* 자리가 차면 멈추는 게 아니라 오래된 것부터 버린다. 어제 것 지키려다
     * 오늘 것을 못 적으면 본말이 뒤집힌다. */
    if (s_jrn_n >= JRN_MAX) {
        uint16_t drop = JRN_MAX / 4;
        memmove(s_jrn, s_jrn + drop, sizeof(jrn_t) * (JRN_MAX - drop));
        s_jrn_n = JRN_MAX - drop;
        if (s_jrn_n) s_jrn[0].flags |= JRN_BREAK;   /* 앞이 잘렸으니 여기도 경계다 */
    }
    /* 처음 40줄은 1분(기울기를 빨리 얻는다), 그 다음 100줄은 5분,
     * 그 뒤는 15분. 200줄로 정확히 하루(40분 + 500분 + 900분 = 24시간)를 덮는다. */
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

/* 🚨 화면을 켜 둔 동안의 소모를 여태 한 번도 못 쟀다. 일지가 시간만 보고
 * 적는데(1분→5분→15분) 화면은 30초면 꺼지니, '화면O' 두 줄이 연달아 나올
 * 수가 없었다. 기울기는 이웃한 두 줄이 둘 다 화면O 여야 나온다 — 그래서
 * 186줄을 쌓고도 "화면O: 표본 부족" 이었다(0909 실기). 가장 크게 먹는
 * 요인이 통째로 측정 밖에 있었다.
 *
 * 이제 켜는 자리와 끄기 직전에 한 줄씩 남긴다. 둘은 이웃이고 둘 다 화면O 라
 * 사이 구간이 곧 화면을 켜 둔 시간이다.
 * 🚨 끄기 '직전' 에 불러야 한다. 끄고 나서 부르면 화면X 로 적혀 짝이 깨진다. */
void port_battery_mark(bool screen_on)
{
    static int64_t last_mark;
    if (port_battery_plugged()) return;          /* 꽂혀 있으면 방전이 아니다 */
    int64_t now = esp_timer_get_time();
    if (last_mark && now - last_mark < 15000000LL) return;   /* 깜빡임 방지 */
    int mv  = port_battery_mv();
    int pct = port_battery_percent();
    if (mv <= 0 || pct < 0) return;
    jrn_load();
    if (s_jrn_n >= JRN_MAX) return;              /* 자리 정리는 평소 경로에 맡긴다 */
    last_mark = now;
    jrn_put(now, mv, pct, screen_on, false);
}

/* ── CPU 가 실제로 얼마나 깨어 있나 ───────────────────────────
 * 배터리로 재려면 케이블을 뽑아야 하지만, "CPU 가 일한 비율"은 꽂아둔 채로
 * 잴 수 있다. 화면이 꺼진 동안 노는 시간이 늘수록 전류가 준다 —
 * 폴링을 줄인 게 실제로 먹혔는지 이걸로 확인한다.
 *
 * 두 시점의 태스크별 누적 실행시간을 빼서 그 사이 구간만 본다. */
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
    if (!span) { ESP_LOGW("cpu", "%s: 잰 구간이 없다", when); return; }

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
    /* 코어가 둘이라 노는 시간의 최대치는 200% 다. 100 으로 환산한다. */
    int idle10 = (int)((uint64_t)idle * 1000 / span / 2);
    ESP_LOGI("cpu", "  ▸ 노는 비율 %d.%d%%  (일한 비율 %d.%d%%)",
             idle10 / 10, idle10 % 10, (1000 - idle10) / 10, (1000 - idle10) % 10);
}


/* ── 건강 검사 ────────────────────────────────────────────────
 * 0907 밤 교훈: "안 뻗었다"는 기계 기준이고 사람 기준은 "화면·터치·소리가
 * 되나"다. 그리기가 1만 3천 번 실패하는 동안에도 워치독은 안 물었고, 나는
 * 그걸 "통과"라고 불렀다. 증상도 원인도 둘 다 못 잡은 것이다.
 *
 * 그래서 두 가지를 둔다:
 *  1) 어떤 오류든 나면 센다 — 내가 미리 생각 못 한 것까지 걸리게
 *  2) 출력이 실제로 나오는지 직접 확인한다 (터치 칩이 대답하나)
 */
#include "esp_log.h"

static volatile uint32_t s_err_n, s_warn_n, s_err_known;
static vprintf_like_t    s_log_next;

/* 🚨 세기만 하면 그것도 현상이다. 터진 순간의 "왜"를 같이 붙잡는다.
 * 그리기 실패는 사실 메모리 문제였는데, 횟수만 세면 그걸 못 본다.
 * 첫 오류가 났을 때의 자원 상태를 찍어두면 증상과 원인이 한 줄에 붙는다. */
static char     s_err_first[56];
static uint32_t s_err_free, s_err_big;
static uint8_t  s_err_ctx;          /* bit0 BLE 연결 · bit1 녹음중 · bit2 화면꺼짐 */

/* 서식을 실제 문장으로 풀어 특정 문구가 있는지 본다 */
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
        /* 🚨 이미 원인을 아는 무해한 오류는 따로 센다. 섞어 세면 개수만
         * 늘어 진짜 오류를 가린다 — 그렇다고 지우지는 않는다. 아는 것만
         * 빼고, 왜 무해한지는 여기 적어둔다.
         *
         * i2s_channel_disable "not been enabled yet":
         *   마이크 코덱 핸들이 송·수신 한 몸이라 닫을 때 안 쓴 송신 채널까지
         *   끄려 든다(로그의 paired out_enable: 0). 녹음 자체는 정상으로
         *   기록된다(0908 검증에서 15건 다 남았다). 벤더 부품 안쪽이라 손 안 댄다. */
        if (fmt_has(fmt, ap, "has not been enabled yet")) { s_err_known++; goto pass; }
        if (s_err_n++ == 0) {
            /* 🚨 서식 문자열만 베끼면 "E (%lu) %s: %s(" 만 남아 쓸모없다
             * (0907 밤에 그렇게 해서 원인을 못 봤다). va_copy 로 안전하게
             * 풀어서 실제 문장을 남긴다. */
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

/* 사람이 보는 것들이 실제로 살아 있나. 0 이면 정상. */
int port_health_check(char *out, size_t len)
{
    int bad = 0;
    char tp[24] = "확인불가";

    /* 터치 칩이 살아있나 — I2C 로 말을 걸어 대답(ACK)하는지만 본다.
     * 🚨 예전엔 체크코드 레지스터(0xD1FC)를 읽어 0x204ECACA 인지 봤는데,
     * 돌아오는 값이 매번 달랐다(0x00200232 = x32 y562 … 좌표였다).
     * 칩이 리포트 모드라 물은 레지스터 대신 터치 데이터를 준다. 락을 걸어도
     * 같았으니 경쟁이 아니라 내가 프로토콜을 잘못 안 것이다. 그 값은 못 믿으니
     * 확실한 것만 본다: 전원이 나갔거나 칩이 죽으면 ACK 자체가 없다. */
    port_lock();
    tp_open();
    if (s_tp) {
        uint8_t reg[2] = { 0xD1, 0xFC };
        if (i2c_master_transmit(s_tp, reg, 2, 200) == ESP_OK) {
            snprintf(tp, sizeof tp, s_tp_asleep ? "재우는중" : "응답함");
        } else if (s_tp_asleep) {
            snprintf(tp, sizeof tp, "재우는중");     /* 자는 중엔 무응답이 정상 */
        } else { snprintf(tp, sizeof tp, "무응답"); bad++; }
    }

    port_unlock();

    if (s_err_n) bad++;

    if (s_err_n) {
        /* 증상(무슨 오류)과 원인(그때 자원이 어땠나)을 한 줄에 붙인다 */
        char *nl = strchr(s_err_first, '\n'); if (nl) *nl = 0;
        snprintf(out, len, "오류 %lu · 터치 %s │ 첫오류 \"%s\" 그때 내부 %luKB 최대덩어리 %luKB%s%s%s",
                 (unsigned long)s_err_n, tp, s_err_first,
                 (unsigned long)(s_err_free / 1024), (unsigned long)(s_err_big / 1024),
                 (s_err_ctx & 1) ? " BLE연결" : "",
                 (s_err_ctx & 2) ? " 녹음중" : "",
                 (s_err_ctx & 4) ? " 화면꺼짐" : "");
    } else {
        snprintf(out, len, "정상 · 경고 %lu%s · 터치 %s", (unsigned long)s_warn_n,
                 s_err_known ? " · 무해(설명됨)" : "", tp);
    }
    return bad;
}

uint32_t port_health_errors(void) { return s_err_n; }
