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
/* 꽂아둔 채로 절전 효과를 재는 실험.
 * 화면 켠 채 20초, 끈 채 20초를 각각 재서 CPU 가 일한 비율을 비교한다.
 * 배터리를 못 재는 상황에서 이게 유일한 객관적 잣대다. */
static void probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));      /* 부팅 소란이 가라앉기를 기다린다 */

    for (int round = 1; round <= 3; round++) {
        port_lock(); launcher_screen_toggle(); port_unlock();   /* 확실히 켠다 */
        vTaskDelay(pdMS_TO_TICKS(1500));
        port_cpu_mark();
        vTaskDelay(pdMS_TO_TICKS(20000));
        port_cpu_report("화면 켜짐 20초");

        port_lock(); launcher_screen_off_manual(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(1500));
        port_cpu_mark();
        vTaskDelay(pdMS_TO_TICKS(20000));
        port_cpu_report("화면 꺼짐 20초");
        ESP_LOGW(TAG, "[probe] %d회차 끝", round);
    }
    ESP_LOGW(TAG, "[probe] 측정 완료");
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_HOSTILE
/* 공격적 검증. 정상 경로가 아니라 경계와 경합을 노린다.
 * 버그는 "천천히 제대로 쓰는" 데 안 살고 "빨리 아무렇게나 쓰는" 데 산다. */
static uint32_t hs_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
/* 🚨 "안 뻗었나"만 보면 안 된다. 그리기가 1만 3천 번 실패하는 동안에도
 * 워치독은 안 물었고 나는 그걸 통과라고 불렀다. 단계마다 사람이 보는
 * 것들이 살아 있는지 확인한다 — 오류가 하나라도 났으면 실패다. */
static int s_hs_fail;

static void hs(const char *n, uint32_t b)
{
    uint32_t now = hs_heap(); long d = (long)now - (long)b;
    char health[64];
    int bad = port_health_check(health, sizeof health);
    if (bad) s_hs_fail++;
    ESP_LOGW(TAG, "[공격] %-28s 힙 %6u (%+ld)  %s%s", n, (unsigned)now, d,
             health, bad ? "   ★★ 실패" : "");
    port_health_begin();        /* 다음 단계는 새로 센다 */
}

static void hostile_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    port_health_begin();
    ESP_LOGW(TAG, "════════ 공격적 검증 시작 힙 %u ════════", (unsigned)hs_heap());
    static const badge_app_t *const A[] = {
        &app_calc, &app_clock, &app_keys, &app_games, &app_meet, &app_mouse,
    };
    const int AN = sizeof(A)/sizeof(A[0]);
    uint32_t h;

    /* 1. 애니메이션이 끝나기도 전에 다음으로 — 화면 전환 경합 */
    h = hs_heap();
    for (int i = 0; i < 150; i++) {
        port_lock(); launcher_open((const badge_app_t *)A[i % AN]); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(60));           /* 열림 애니메이션보다 짧게 */
        port_lock(); launcher_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    hs("앱 난폭 전환 x150 (60ms)", h);

    /* 2. 전환 도중에 화면을 끄고 켠다 */
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
    hs("전환 도중 화면 껐다켜기 x60", h);

    /* 3. 녹음 API 를 엉터리 순서로 부른다 */
    h = hs_heap();
    port_rec_stop();                       /* 안 하는데 멈춰라 */
    ESP_LOGW(TAG, "[공격] 안 켰는데 stop → active=%d", port_rec_active());
    if (port_rec_start(0)) {
        vTaskDelay(pdMS_TO_TICKS(800));
        bool twice = port_rec_start(1);    /* 도는데 또 시작 */
        ESP_LOGW(TAG, "[공격] 도는데 또 start → %s", twice ? "★받아들임(문제)" : "거절됨(정상)");
        port_rec_stop(); port_rec_stop();  /* 두 번 멈춤 */
        for (int i = 0; i < 20 && port_rec_active(); i++) vTaskDelay(pdMS_TO_TICKS(300));
    }
    hs("녹음 엉터리 순서", h);

    /* 4. 녹음하면서 앱을 갈아탄다 — 녹음은 살아 있어야 한다 */
    h = hs_heap();
    if (port_rec_start(0)) {
        for (int i = 0; i < 12; i++) {
            port_lock(); launcher_open((const badge_app_t *)A[i % AN]); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(400));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        ESP_LOGW(TAG, "[공격] 앱 갈아타는 동안 녹음 %s (%lu초)",
                 port_rec_active() ? "살아있음(정상)" : "★죽음(문제)",
                 (unsigned long)port_rec_seconds());
        port_rec_stop();
        for (int i = 0; i < 20 && port_rec_active(); i++) vTaskDelay(pdMS_TO_TICKS(300));
    }
    hs("녹음 중 앱 전환 x12", h);

    /* 5. 디렉터리 넘치기 — 슬롯이 12개다. 15번 녹음해 밀려나는지 본다 */
    h = hs_heap();
    for (int i = 0; i < 15; i++) {
        if (!port_rec_start(0)) { ESP_LOGW(TAG, "[공격] %d번째 녹음 거절됨", i + 1); break; }
        vTaskDelay(pdMS_TO_TICKS(1200));
        port_rec_stop();
        for (int k = 0; k < 20 && port_rec_active(); k++) vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "[공격] 15회 녹음 뒤 보관 목록 확인 (남은 자리 %lu분)",
             (unsigned long)(port_rec_free_seconds() / 60));
    hs("슬롯 넘치기 x15", h);

    /* 6. BLE 를 빠르게 올렸다 내렸다 */
    h = hs_heap();
    for (int i = 0; i < 8; i++) {
        port_hid_start(); vTaskDelay(pdMS_TO_TICKS(400));
        port_hid_stop();  vTaskDelay(pdMS_TO_TICKS(400));
    }
    hs("BLE 난폭 토글 x8", h);

    /* 7. 설정 저장을 두들긴다 (NVS) */
    h = hs_heap();
    for (int i = 0; i < 40; i++) {
        port_brightness_set(20 + (i % 60));
        badge_display_set_xgap((i % 2) ? 8 : 6);
        vTaskDelay(pdMS_TO_TICKS(30));   /* 양보 없이 돌리면 워치독이 문다 */
    }
    badge_display_set_xgap(8);
    port_brightness_set(45);
    hs("밝기·여백 40회 변경", h);

    ESP_LOGW(TAG, "════════ 공격적 검증 끝 힙 %u — 실패 단계 %d개 ════════",
             (unsigned)hs_heap(), s_hs_fail);
    vTaskDelete(NULL);
}
#endif


#ifdef BADGE_APPBENCH
/* ── 앱을 스스로 돌려보는 벤치 ────────────────────────────────
 * 🚨 손으로 만져야만 알 수 있던 것들(프레임 시간, 기울기 기준)을 기기가
 * 혼자 재게 한다. 시뮬은 x86 이라 시간이 150배 다르고, IMU 는 아예 없어서
 * 기울기로 도는 것은 시뮬에서 한 번도 시험된 적이 없었다. */
extern void games_debug_play_bricks(void);
extern void games_debug_play_orb(void);
extern void games_debug_tilt0(float *base, int *set);
extern void orb_set_lon(float lon);
#include <math.h>

static void appbench_task(void *arg)
{
    (void)arg;
    /* IMU 가 잠들 때까지 기다린다(안 쓴 지 5초면 잔다). 브릭 첫판이
     * 안 먹던 상황을 그대로 만든다. */
    vTaskDelay(pdMS_TO_TICKS(9000));
    ESP_LOGW(TAG, "════════ 앱 벤치 시작 ════════");

    float ax = 0, ay = 0, az = 0;
    bool  ok = port_imu_accel3(&ax, &ay, &az);
    ESP_LOGW(TAG, "재우고 난 뒤 첫 읽기: %s x=%.0f y=%.0f z=%.0f",
             ok ? "믿을 만함" : "아직 모름", ax, ay, az);

    /* ① 브릭 — 기울기 기준이 지금 자세와 맞게 잡히나 */
    port_lock(); launcher_open(&app_games); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(500));
    port_lock(); games_debug_play_bricks(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));
    float base = 0; int set = 0;
    games_debug_tilt0(&base, &set);
    port_imu_accel3(&ax, &ay, &az);
    ESP_LOGW(TAG, "브릭 기울기 기준 %s base=%.1f · 1.5초 뒤 실제 y=%.1f · 차이 %.1f",
             set ? "잡힘" : "★안잡힘", base, ay, ay - base);
    if (set && fabsf(ay - base) > 60.0f)
        ESP_LOGE(TAG, "★ 기준이 실제와 %.0f 나 벌어졌다 — 판이 끝에 붙는다", ay - base);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ② 물 — 프레임 시간 */
    port_lock(); launcher_home(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    port_lock(); launcher_open(&app_water); port_unlock();
    ESP_LOGW(TAG, "── 물 12초 ──");
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* ③ 천체 — 프레임 시간 (계속 도는 상태로 둔다) */
    port_lock(); launcher_home(); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(600));
    port_lock(); launcher_open(&app_games); port_unlock();
    vTaskDelay(pdMS_TO_TICKS(400));
    port_lock(); games_debug_play_orb(); port_unlock();
    ESP_LOGW(TAG, "── 천체 12초 ──");
    for (int i = 0; i < 12; i++) {
        port_lock(); orb_set_lon((i % 10) * 0.1f); port_unlock();   /* 계속 움직이게 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    port_lock(); launcher_home(); port_unlock();
    ESP_LOGW(TAG, "════════ 앱 벤치 끝 ════════");
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_BTNTEST
/* 밤새 혼자 돌리는 검증. 사람이 못 보는 사이에 도는 것이라, "안 뻗었다"가
 * 아니라 "사람이 보는 것이 실제로 바뀌었나"를 본다. */
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
    ESP_LOGW(TAG, "[버튼] %-34s %s%s%s", name, ok ? "통과" : "★ 실패",
             detail && detail[0] ? " — " : "", detail ? detail : "");
}

static void btntest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(9000));
    ESP_LOGW(TAG, "════════ 버튼·소리 검증 시작 ════════");

    /* ── 1. BOOT 눌림이 화면을 실제로 토글하나 ──────────────────
     * ⚠️ GPIO0 은 스트랩 핀이라 화면이 켜져 있을 때만 짧게 누른다.
     *    (화면이 꺼지면 라이트슬립에 들어가고, 그때 리셋이 겹치면
     *     다운로드 모드로 들어갈 수 있다.) */
    char d[80];
    for (int i = 0; i < 4; i++) {
        bool before = launcher_screen_is_off();
        uint32_t n0 = port_boot_isr_count();
        port_boot_btn_fake(60);
        vTaskDelay(pdMS_TO_TICKS(700));
        bool after = launcher_screen_is_off();
        uint32_t dn = port_boot_isr_count() - n0;
        snprintf(d, sizeof d, "화면 %s→%s · 인터럽트 %lu회",
                 before ? "꺼짐" : "켜짐", after ? "꺼짐" : "켜짐", (unsigned long)dn);
        /* 인터럽트가 딱 한 번 물고, 화면 상태가 뒤집혔어야 한다.
         * 여러 번 물면 레벨 인터럽트가 폭주한 것이다. */
        bt("BOOT 눌림 → 화면 토글", (after != before) && dn == 1, d);

        /* 다음 회차는 화면이 켜진 상태에서 시작해야 한다(스트랩 위험 회피) */
        if (launcher_screen_is_off()) {
            port_boot_btn_fake(60);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }

    /* ── 2. 아주 짧은 눌림도 잡히나 (원래 놓치던 그 경우) ───────── */
    {
        bool before = launcher_screen_is_off();
        uint32_t n0 = port_boot_isr_count();
        port_boot_btn_fake(25);
        vTaskDelay(pdMS_TO_TICKS(700));
        snprintf(d, sizeof d, "인터럽트 %lu회",
                 (unsigned long)(port_boot_isr_count() - n0));
        bt("25ms 짧은 눌림도 잡히나", launcher_screen_is_off() != before, d);
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 3. 연타에도 폭주하지 않나 ───────────────────────────── */
    {
        uint32_t n0 = port_boot_isr_count();
        for (int i = 0; i < 6; i++) { port_boot_btn_fake(50); vTaskDelay(pdMS_TO_TICKS(400)); }
        uint32_t dn = port_boot_isr_count() - n0;
        snprintf(d, sizeof d, "6번 눌러 인터럽트 %lu회", (unsigned long)dn);
        bt("연타에 인터럽트 폭주 안 하나", dn <= 8, d);
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 4. 소리를 꺼두면 코덱을 안 잡나 ─────────────────────── */
    {
        port_health_begin();
        port_tone_volume(0);                 /* 설정의 소리 OFF 와 같은 상태 */
        for (int i = 0; i < 40; i++) {
            port_tone_freq(800 + i * 10);
            port_tone_enable(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            port_tone_enable(false);
        }
        port_tone_hold(true);                /* 게임이 붙잡으려 해도 */
        vTaskDelay(pdMS_TO_TICKS(500));
        bool held_open = port_tone_codec_open();
        port_tone_hold(false);
        snprintf(d, sizeof d, "코덱 %s", held_open ? "열렸다" : "안 열렸다");
        bt("소리 OFF 면 코덱 안 잡나", !held_open, d);
    }

    /* ── 5. 소리를 켜면 다시 나나 (4번이 과잉이면 여기서 걸린다) ── */
    {
        port_tone_volume(60);
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        bool open = port_tone_codec_open();
        port_tone_freq(1000); port_tone_enable(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        port_tone_enable(false);
        port_tone_hold(false);
        snprintf(d, sizeof d, "코덱 %s", open ? "열렸다" : "안 열렸다");
        bt("소리 ON 이면 코덱 잡나", open, d);
    }

    /* ── 6. 붙잡아도 조용하면 스스로 놓나 (전력) ───────────────── */
    {
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        bool open_now = port_tone_codec_open();
        vTaskDelay(pdMS_TO_TICKS(23000));    /* 20초 문턱보다 길게 */
        bool open_later = port_tone_codec_open();
        port_tone_hold(false);
        snprintf(d, sizeof d, "잡을 땐 %s · 23초 뒤 %s",
                 open_now ? "열림" : "닫힘", open_later ? "열림" : "닫힘");
        bt("붙잡아도 20초 조용하면 놓나", open_now && !open_later, d);
    }

    /* ── 7. 화면이 꺼지면 코덱을 놓나 (게임 켠 채 화면 꺼짐) ────── */
    {
        port_tone_volume(60);
        port_tone_hold(true);
        vTaskDelay(pdMS_TO_TICKS(200));
        bool before = port_tone_codec_open();
        port_lock(); launcher_screen_toggle(); port_unlock();   /* 화면 끔 */
        vTaskDelay(pdMS_TO_TICKS(400));
        port_tone_hold(false);               /* 앱이 화면 꺼짐을 보고 놓는 것과 같다 */
        vTaskDelay(pdMS_TO_TICKS(4000));     /* 3초 문턱 지나게 */
        bool after = port_tone_codec_open();
        snprintf(d, sizeof d, "끄기 전 %s · 끈 뒤 %s",
                 before ? "열림" : "닫힘", after ? "열림" : "닫힘");
        bt("화면 꺼지면 코덱 놓나", before && !after, d);
        /* 화면을 다시 켜둔다 */
        if (launcher_screen_is_off()) { port_boot_btn_fake(60); vTaskDelay(pdMS_TO_TICKS(700)); }
    }

    /* ── 8. LVGL 20ms 타이머가 실기에서 실제로 몇 ms 마다 오나 ──────
     * 게임은 이 타이머로 공을 움직인다. 옛 방식은 "불릴 때마다 한 걸음"
     * 이라 타이머가 밀리면 공이 그만큼 느려졌다. 흐른 시간을 재서 걸음을
     * 나누도록 고쳤는데, 그러면 실효 속도가 올라간다 — 얼마나 올라가는지
     * 알아야 공 속도를 다시 맞출지 정할 수 있다. */
    {
        s_jit_n = 0; s_jit_sum = 0; s_jit_max = 0; s_jit_last = 0;
        lv_timer_t *jt;
        port_lock(); jt = lv_timer_create(jit_cb, 20, NULL); port_unlock();
        /* 그냥 놀 때가 아니라 화면이 바쁠 때를 재야 의미가 있다 */
        static const badge_app_t *const J[] = { &app_calc, &app_clock, &app_keys };
        for (int i = 0; i < 24; i++) {
            port_lock(); launcher_open(J[i % 3]); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        port_lock(); lv_timer_delete(jt); port_unlock();
        uint32_t avg = s_jit_n ? s_jit_sum / s_jit_n : 0;
        snprintf(d, sizeof d, "평균 %lums · 최대 %lums (표본 %lu)",
                 (unsigned long)avg, (unsigned long)s_jit_max, (unsigned long)s_jit_n);
        /* 이건 최악 부하다. 통과/실패로 볼 값이 아니라 알아야 할 값이다. */
        bt("타이머 간격(앱 마구 전환 = 최악)", true, d);
    }

    /* ── 9. 진짜 게임이 도는 동안의 타이머 간격 ─────────────────
     * 공 속도를 정하는 건 이 값이다. 8번(앱 전환)은 실제 노는 상황이 아니다. */
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
        snprintf(d, sizeof d, "평균 %lums · 최대 %lums (표본 %lu) → 옛 방식 실효속도 %lu%%",
                 (unsigned long)avg, (unsigned long)s_jit_max, (unsigned long)s_jit_n,
                 avg ? (unsigned long)(2000 / avg) : 0);
        bt("타이머 간격(벽돌깨기 실제)", avg > 0, d);
    }

    char health[80];
    int bad = port_health_check(health, sizeof health);
    ESP_LOGW(TAG, "════════ 버튼·소리 검증 끝 — 실패 %d개 · %s ════════",
             s_bt_fail + (bad ? 1 : 0), health);
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_SELFTEST
/* 전체 자가검사. 사람 손 없이 돌려서 검증한다.
 * 각 단계마다 내부 힙을 남겨 누수가 있으면 눈에 띄게 한다. */
static uint32_t st_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static void st_step(const char *name, uint32_t before)
{
    uint32_t now = st_heap();
    long d = (long)now - (long)before;
    ESP_LOGW(TAG, "[검사] %-26s 힙 %6u  (%+ld)%s",
             name, (unsigned)now, d, (d < -2000) ? "   ★샘" : "");
}

static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    /* 두 번 돌린다. 1회차와 2회차를 비교하면 "첫 회 일회성"과 "진짜 누수"가
     * 갈린다 — 2회차도 계속 줄면 그건 새는 것이다. */
    for (int pass = 1; pass <= 1; pass++) {
    uint32_t h0 = st_heap();
    ESP_LOGW(TAG, "════════ 자가검사 %d회차 시작  내부힙 %u ════════", pass, (unsigned)h0);

    /* BLE 를 올려두고 유지한다. 밖에서 스캔해 잡히는지 보려는 것. */
    ESP_LOGW(TAG, "[검사] BLE 광고 시작 — 3분 유지");
    port_hid_start();
    for (int i = 0; i < 180; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i % 15 == 0)
            ESP_LOGW(TAG, "[검사] BLE %s  상대=%s  간격 %dms",
                     port_hid_connected() ? "연결됨" : "광고중",
                     port_hid_peer() ? port_hid_peer() : "-",
                     port_hid_interval_ms());
        /* 붙어 있으면 실제로 리포트를 쏜다. 밖에서 btmon 으로 보면
         * ATT 알림이 찍힌다 — HID 가 진짜 나가는지 확인용. */
        if (port_hid_connected()) {
            port_hid_mouse(20, 0, 0, 0);   vTaskDelay(pdMS_TO_TICKS(60));
            port_hid_mouse(-20, 0, 0, 0);  vTaskDelay(pdMS_TO_TICKS(60));
            if (i % 10 == 0) { port_hid_key(0, 0x04); }   /* 'a' */
        }
    }
    ESP_LOGW(TAG, "════════ %d회차 끝  내부힙 %u (시작 대비 %+ld) ════════",
             pass, (unsigned)st_heap(), (long)st_heap() - (long)h0);
    }   /* pass 반복 끝 */
    vTaskDelete(NULL);
}
#endif

#ifdef BADGE_SOAK_DISPLAY
/* 사용자가 겪은 그대로 재현한다:
 *   화면 끄기 -> 켜기(잠금화면) -> 손잡이로 잠금해제(홈) -> 다시 끄기 ...
 * 화면만 껐다 켜는 것보다 훨씬 넓은 경로다 — 화면 전환 애니메이션, 손잡이
 * 객체 생성/삭제, 잠금화면 캐시가 전부 얽힌다. 0906 패닉도 여기서 났다. */
static void soak_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGW(TAG, "[soak] 껐다켜고 잠금해제 120회 시작");
    for (int i = 1; i <= 120; i++) {
        port_lock(); launcher_screen_off_manual(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));

        port_lock(); launcher_screen_toggle(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        /* 잠금해제 = 손잡이가 하는 일 그대로 */
        port_lock(); launcher_show_home(); port_unlock();
        vTaskDelay(pdMS_TO_TICKS(600));

        /* 가끔 앱까지 들어갔다 나온다 — close_done_cb 가 손잡이 포인터를
         * NULL 로 만드는 그 경로가 0906 패닉의 현장이었다. */
        if (i % 5 == 0) {
            port_lock(); launcher_open(&app_calc); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
            port_lock(); launcher_home(); port_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        if (i % 10 == 0)
            ESP_LOGW(TAG, "[soak] %3d/120 통과  내부힙 %u", i,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    ESP_LOGW(TAG, "[soak] 120회 전부 통과 — 껐다켜고 잠금해제 경로 이상 없음");
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

    /* esp_hid 는 연결·끊김을 기본 이벤트 루프로 보낸다. 루프가 없으면
     * 콜백이 아예 안 불리고, 끊겨도 끊긴 줄 모른다. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 디스플레이는 우리가 직접 올린다 — 패널 핸들을 쥐어야 화면을 진짜로
     * 끌 수 있다. BSP 의 bsp_display_start_with_config() 는 핸들을 숨긴다.
     * 자세한 사정은 display.c 머리말. */
    if (!badge_display_start()) {
        ESP_LOGE(TAG, "display start 실패");
        return;
    }

    /* 기본 30ms 로 읽으면 마우스가 뚝뚝 끊겨 보인다. 촘촘하게 읽는다. */
    lv_indev_t *indev = badge_display_indev();
    if (indev) lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    port_rtc_restore();
    port_time_autosync();
    settings_load();      /* 저장해둔 밝기·소리·꺼짐시간 */
    /* 설정은 읽되 화면은 계속 어둡게 둔다. 아직 GRAM 에 쓰레기가 들어 있다 —
     * 여기서 밝히면 부팅 때 번쩍하던 그것이 그대로 재현된다. */
    badge_display_brightness(0);

    /* 라이트슬립 — 0906 에 켰다가 "화면 껐다 켜면 재부팅"이 나서 껐고,
     * 0907 에 원인을 막고 다시 켰다. 원인은 PSRAM 이었다: LVGL 태스크 스택이
     * PSRAM 에 있는데 라이트슬립이 PSRAM 을 반쯤 재운다. 깨어날 때 그 메모리가
     * 준비되기 전에 태스크가 돌면 그대로 뻗는다.
     *   (a) LVGL 스택을 내부 RAM 으로 (display.c 의 stack_in_psram=false)
     *   (b) CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED 끄기
     * 둘 다 막았다. BLE 연결·I2S 녹음 중엔 port_pm_hold() 로 못 자게 잡는다.
     *
     * 이게 남은 유일한 큰 지렛대다 — 화면이 꺼지면 CPU 는 이미 99.1% 놀고
     * 있는데도 전기를 먹는다(0907 실측). 일해서가 아니라 깨어 있어서다. */
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "전원관리 설정 실패");

    bsp_display_lock(UINT32_MAX);
    splash_show();              /* 홈은 스플래시가 끝나면서 뜬다 */
    bsp_display_unlock();

    /* 첫 프레임이 화면에 올라간 뒤에 밝기를 올린다. 이 순서 덕에 부팅 때
     * 초기화 안 된 GRAM 이 번쩍하는 게 사라진다. */
    vTaskDelay(pdMS_TO_TICKS(80));
    badge_display_brightness(port_brightness_get());

    /* 왜 재부팅됐는지. 브라운아웃(배터리 바닥)인지 뻗은 건지가 여기서 갈린다.
     * 시리얼을 못 보는 상황에서도 알아야 하니 다음 부팅 때까지 남긴다. */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        static const char *NAME[] = {
            "알수없음", "전원켜짐", "외부리셋", "소프트리셋", "패닉",
            "인터럽트워치독", "태스크워치독", "기타워치독", "딥슬립복귀",
            "브라운아웃", "SDIO", "USB리셋", "JTAG리셋", "eFuse오류",
            "전원글리치", "CPU잠김",
        };
        const char *n = (rr < sizeof(NAME)/sizeof(NAME[0])) ? NAME[rr] : "?";
        if (rr == ESP_RST_BROWNOUT)      ESP_LOGE(TAG, "재부팅 사유: 브라운아웃 — 전압이 주저앉았다 (배터리 바닥 의심)");
        else if (rr == ESP_RST_PWR_GLITCH) ESP_LOGE(TAG, "재부팅 사유: 전원 글리치 — 레일이 순간 흔들렸다");
        else if (rr == ESP_RST_CPU_LOCKUP) ESP_LOGE(TAG, "재부팅 사유: CPU 잠김");
        else if (rr == ESP_RST_PANIC)    ESP_LOGE(TAG, "재부팅 사유: 패닉 — 코드가 뻗었다");
        else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT)
                                         ESP_LOGE(TAG, "재부팅 사유: 워치독 — 어딘가 CPU 를 안 놓았다");
        else                             ESP_LOGI(TAG, "재부팅 사유: %s (%d)", n, (int)rr);
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
    /* 임시 검증: 화면을 껐다 켜는 경로를 사람 손 없이 두들긴다.
     * 여기가 라이트슬립 때 재부팅이 나던 자리라, 패널을 실제로 끄도록
     * 바꾼 뒤에도 멀쩡한지 확인해야 한다. */
    xTaskCreate(soak_task, "soak", 4096, NULL, 3, NULL);
#endif
    /* 꽂힌 채 부팅했으면 처음부터 안 재운다 — 안 그러면 런처 타이머가 도는
     * 60초 동안 로그가 끊긴다. */
    if (port_battery_plugged()) port_pm_hold(true);

    badge_creds_init();            /* secrets.h 에 값이 있으면 배지에 새로 넣는다 */
    port_battery_journal_dump();   /* 지난밤 기록이 있으면 여기 뱉는다 */
    port_heap_report("boot");
    ESP_LOGI(TAG, "부팅 화면 → 런처");
}
