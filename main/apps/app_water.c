/* 물 — 진짜 입자 유체. 배 한 척 띄웠다.
 *
 * 🚨 세 번 엎었다.
 *   1) 고정 사인파를 기울인 판에 얹었다 → 판때기였다.
 *   2) 정상파 두 개를 흉내냈다 → 통을 어떻게 흔들든 정해진 모양뿐이었다.
 *   3) 얕은 물 방정식(가로 칸마다 높이 하나) → 물리는 맞았지만 여전히
 *      "파형"이었다. 한 x 에 y 가 하나뿐이라 방울도, 말려 덮치는 파도도,
 *      떨어져 나온 덩어리도 구조적으로 표현할 자리가 없다. 높이함수인 한
 *      아무리 방정식을 고쳐도 물이 될 수 없다.
 *
 * 그래서 물을 입자로 만든다. 620개의 물방울이 서로 밀치며 통 안에서 논다.
 * 방울·파도·덩어리·튀김이 따로 만든 게 아니라 전부 같은 입자에서 나온다.
 *
 * 방법은 PBF(Position Based Fluids)다. 힘이 아니라 위치를 직접 고친다:
 *   1. 중력으로 일단 옮겨 본다
 *   2. 이웃을 세어 밀도를 잰다 — 너무 빽빽하면 서로 밀어내고, 성기면 당긴다
 *   3. 그 보정을 두세 번 반복해 밀도를 고르게 맞춘다
 *   4. 실제로 움직인 거리에서 속도를 되뽑는다
 * 힘으로 밀면 시간 간격이 조금만 커져도 터지는데, 위치를 고치는 방식은
 * 안 터진다. 실시간 유체에 이 방식을 쓰는 이유다.
 *
 * 비용: 입자 620개 x 이웃 12개 x 3회 ≈ 프레임당 25만 번. 240MHz 면 감당된다.
 * 이웃 찾기는 격자로 나눠 제 칸과 이웃 칸만 본다 — 전부 대보면 620x620 이다.
 *
 * 🚨 그림은 466x466 RGB565 버퍼(434KB, PSRAM)에 우리가 직접 찍어 한 장으로
 * 넘긴다. 0909 에 바꿨다 — 그 전에는 "LVGL 이 16줄 밴드를 그릴 때 끼어들어
 * 그 안에만 그린다" 였는데, 밴드가 30개라 콜백이 프레임당 30번 불리고 그때마다
 * 466열을 훑어 lv_draw_rect 를 2만 번 넘게 불렀다. 프레임 간격 311ms(초당
 * 3장)의 정체가 그것이었고, 그 느린 프레임이 물리의 한 걸음을 33ms 로 키워
 * 물을 통째로 위로 날려버렸다.
 *
 * 🚨 예전 주석은 "PSRAM 캔버스는 0908 에 그리기가 13,943번 실패한 그 함정"
 * 이라며 이 방식을 금지했다. 그 함정은 이것과 다르다 — 그때 문제는 LVGL 의
 * **그리기 버퍼**를 PSRAM 에 둬서 SPI 가 DMA 를 못 하고 전송마다 내부 바운스
 * 버퍼를 잡다 실패한 것이다. 이 그림은 화면으로 밀어내는 버퍼가 아니라 CPU 가
 * 읽는 **원본**이다. LVGL 이 여기서 읽어 16줄짜리 내부 RAM 버퍼에 옮기고,
 * DMA 는 그 내부 버퍼에서 나간다 — DMA 경로는 하나도 안 바뀐다.
 */
#include "app.h"
#include "port.h"
#include "water.h"
#include <math.h>
#include <string.h>
#ifdef BADGE_SIM
/* 시뮬엔 PSRAM 도 esp_timer 도 없다. 같은 이름으로 대체를 둔다. */
#  include <stdlib.h>
#  include <stdio.h>
#  include <sys/time.h>
#  define MALLOC_CAP_SPIRAM 0
#  define MALLOC_CAP_INTERNAL 0
#  define MALLOC_CAP_8BIT 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
static int64_t esp_timer_get_time(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
#  define ESP_LOGI(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#  define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#else
#  include "esp_heap_caps.h"
#  include "esp_timer.h"
#  include "esp_log.h"
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "freertos/semphr.h"
#endif

#define CX      233
#define CY      233
#define R       222         /* 물이 담기는 원 */
#define DEG2RAD 0.0174533f

/* ── 입자 ────────────────────────────────────────────────────
 * 620개로 반원을 채우면 간격이 11px 쯤 된다. 이웃 반경은 그 두 배로 잡아야
 * 서로를 충분히 느낀다 — 좁으면 물이 아니라 모래처럼 흩어진다. */
/* 🚨 620 → 460 으로 줄였던 것을 되돌린다(0909). 줄인 이유는 물리가 프레임을
 * 잡아먹어서였는데, 이제 물리가 코어1 로 가서 그리기와 겹쳐 돈다 — 프레임에
 * 안 붙는다. 위 주석의 "620개로 반원을 채우면 간격이 11px" 이 원래 설계다. */
/* 🚨 입자 수가 곧 물리 값이다 — 비용이 입자 수의 제곱에 비례한다(각 입자가
 * 이웃을 보는데, 같은 통에 입자가 늘면 이웃도 같이 는다). 0909 실측:
 *     620개 → 걸음 하나 27ms.  실시간엔 걸음 8개, 즉 216ms 가 든다.
 * 코어1 예산은 프레임(80ms) 만큼이라 620개로는 걸음을 1~2개밖에 못 넣는다 —
 * 그게 슬로모션의 정체다. 380개면 걸음 하나가 9ms 라 8개가 예산에 들어간다.
 * 대신 점성을 0.22 로 올려 덩어리감은 지켰다. */
#define NP      380
#define HR      22.0f                   /* 이웃 반경 */
#define INV_HR  (1.0f / HR)
#define HR2     (HR * HR)

/* 🚨 이걸 static 으로 두면 앱을 안 켜도 내부 RAM 27KB 를 상시 물고 있다
 * (내부 RAM 은 통틀어 116KB 다). 들어올 때 PSRAM 에서 잡고 나갈 때 놓는다.
 * PSRAM 이어도 되는 이유는 이게 화면으로 밀어내는 버퍼가 아니라 CPU 가
 * 읽고 쓰는 데이터라서다 — DMA 함정과는 상관없다. */
static float *s_x, *s_y;                /* 지금 위치 — 물리가 쓴다 */
static float *s_px, *s_py;              /* 옮기기 전 위치 */
static float *s_vx, *s_vy;

/* 🚨 그리기가 보는 사본. 물리가 코어1 에서 도는 동안 코어0 이 이걸 읽는다 —
 * 같은 배열을 한쪽이 쓰는 중에 다른 쪽이 읽으면 화면이 찢어지거나 죽는다.
 * 한 판이 끝난 자리에서만 베끼므로 그 사이에 아무도 안 건드린다.
 * 시뮬엔 코어가 하나뿐이라 그냥 같은 자리를 가리킨다(베끼지 않는다). */
static float *s_rx, *s_ry, *s_rvx, *s_rvy;

/* ── 물리를 코어1 로 ──────────────────────────────────────────
 * 🚨 코어가 둘인데 무거운 건 전부 코어0 에 몰려 있었다(메인도 BLE 도 CPU0).
 * 물리는 그리기와 아무 상관 없는 계산이라 옆으로 빼면 프레임에서 통째로
 * 빠진다. 0909 실측 110ms 중 입자가 37ms 였다.
 *
 * 겹치는 방식: 코어0 이 N 판을 그리는 동안 코어1 이 N+1 판을 푼다.
 *   1. 지난 판이 끝나기를 기다린다(done)
 *   2. 그 자리에서 사본을 뜬다 — 이때는 아무도 안 건드린다
 *   3. 다음 판 조건을 넘기고 go 를 준다
 *   4. 코어0 은 사본으로 그린다
 * 🚨 사본 없이 같은 배열을 읽으면 한쪽이 쓰는 중에 읽어 화면이 찢어진다.
 * 🚨 나갈 때 반드시 판이 끝난 걸 보고 배열을 놓아야 한다. 거꾸로 하면
 *    코어1 이 이미 놓은 자리를 만진다. */
static struct {
    float gx, gy, kx, ky, burst, sdt;
    int   sub;
} s_pin;
static volatile bool s_still_flag;

static void fluid_step(float dt, float gx, float gy);
static bool water_is_still(void);

/* 한 판. 어느 코어에서 불리든 같은 일을 한다. */
static volatile uint32_t s_phys_us;   /* 지난 판이 걸린 시간 */
static volatile int      s_phys_sub;  /* 그때 돈 걸음 수 */

static void phys_round(void)
{
    int64_t _p0 = esp_timer_get_time();
    if (s_pin.kx || s_pin.ky || s_pin.burst) {
        static uint32_t seed = 987654321u;
        for (int i = 0; i < NP; i++) {
            s_vx[i] += s_pin.kx;
            s_vy[i] += s_pin.ky;
            if (s_pin.burst > 0) {
                /* 정면으로 찌르면 방향이 없다 — 알갱이마다 아무 쪽으로 튄다 */
                seed = seed * 1103515245u + 12345u;
                float a = (float)((seed >> 9) & 0xFFFF) / 65535.0f * 6.2831853f;
                s_vx[i] += cosf(a) * s_pin.burst;
                s_vy[i] += sinf(a) * s_pin.burst;
            }
        }
    }
    for (int k = 0; k < s_pin.sub; k++) fluid_step(s_pin.sdt, s_pin.gx, s_pin.gy);
    s_still_flag = water_is_still();
    s_phys_us  = (uint32_t)(esp_timer_get_time() - _p0);
    s_phys_sub = s_pin.sub;
}

#ifndef BADGE_SIM
static SemaphoreHandle_t s_go, s_done;
static TaskHandle_t      s_phys;
static bool              s_inflight;

static void phys_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        phys_round();
        xSemaphoreGive(s_done);
    }
}

/* 돌고 있는 판이 끝나기를 기다린다. 나갈 때도 이걸 먼저 부른다. */
static void phys_wait(void)
{
    if (!s_inflight) return;
    /* 🚨 넉넉히 기다리되 영원히는 아니다. 코어1 이 막히면 화면까지 멎는다. */
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    s_inflight = false;
}
#else
static void phys_wait(void) { }
#endif

/* 이웃 찾기용 격자 — 제 칸과 둘레 8칸만 본다 */
#define GC      24                      /* 한 변의 칸 수 */
#define GS      (466.0f / GC)            /* 칸 크기(≈19px) */
static uint16_t s_head[GC * GC];        /* 칸마다 첫 입자 (0xFFFF = 빈칸) — 1.1KB 라 남겨둔다 */
static uint16_t *s_next;                /* 같은 칸의 다음 입자 */

static lv_obj_t   *s_root, *s_field, *s_hint;
static lv_timer_t *s_loop;
static float s_gx, s_gy;                /* 중력 방향(화면 좌표) */
static float s_boat_x, s_boat_vx;
static float s_boat_y, s_boat_vy;
/* 중력 방향의 단위벡터. 물·배·그리기가 같은 '아래' 를 봐야 한다. */
static float s_gux = 0.0f, s_guy = 1.0f;

/* 화면 좌표 ↔ '중력이 아래인 틀'. 배를 돌려 태우려고 둔다.
 * A = 중력 방향(아래가 +), C = 그 직각 방향. 중력이 화면 아래면 그대로다. */
#define G_ALONG(px, py)  (((px) - CX) * s_gux + ((py) - CY) * s_guy)
#define G_CROSS(px, py)  (((px) - CX) * s_guy - ((py) - CY) * s_gux)
static float s_fake_gx, s_fake_gy;
static uint32_t s_last_ms;

/* ── 이웃 격자 ─────────────────────────────────────────────
 * 전부 대보면 620x620 이다. 칸으로 나눠 제 칸과 둘레 8칸만 본다. */
static void grid_build_from_current(void)
{
    memset(s_head, 0xFF, sizeof s_head);
    for (int i = 0; i < NP; i++) {
        int cx = (int)(s_x[i] / GS), cy = (int)(s_y[i] / GS);
        if (cx < 0) cx = 0;
        if (cx >= GC) cx = GC - 1;
        if (cy < 0) cy = 0;
        if (cy >= GC) cy = GC - 1;
        int c = cy * GC + cx;
        s_next[i] = s_head[c];
        s_head[c] = (uint16_t)i;
    }
}

/* 통 안으로 되돌린다 */
static inline void clamp_circle(float *x, float *y)
{
    float dx = *x - CX, dy = *y - CY;
    float d2 = dx * dx + dy * dy;
    if (d2 > (float)(R - 2) * (R - 2)) {
        float d = sqrtf(d2);
        float k = (R - 2) / d;
        *x = CX + dx * k;
        *y = CY + dy * k;
    }
}

/* ── 물 한 걸음 ─────────────────────────────────────────────
 * 🚨 처음엔 PBF 의 정석 커널(poly6/spiky)로 짰는데, 완화 상수의 스케일이
 * 우리 단위(픽셀)와 안 맞아 보정이 사실상 0 이 됐다 — 입자가 바닥에
 * 눌러앉아 얇게 깔렸다(0909 실측). 상수를 맞추는 게 까다로운 방식이다.
 *
 * 그래서 이중 밀도 완화(Clavet)로 간다. 게임용으로 만들어진 방식이라
 * 스케일에 둔감하고 안 터진다. 두 가지 밀도를 쓴다:
 *   ρ      보통 밀도 — 기준보다 빽빽하면 밀어내고 성기면 당긴다(표면장력)
 *   ρ_near 아주 가까운 것만 세는 밀도 — 절대 겹치지 않게 밀기만 한다
 * 두 번째가 있어야 입자가 한 점에 뭉쳐 무너지지 않는다. */
#define KSTIFF   26000.0f       /* 보통 밀도의 세기 */
#define KNEAR    78000.0f       /* 가까운 밀도의 세기 — 겹침을 막는다 */
static float s_rho0;            /* 잔잔할 때의 밀도 */

/* 🚨 이웃을 두 번 훑고 있었다. 한 번은 밀도를 세고, 한 번은 그 힘으로 민다.
 * 그런데 그 사이에 자리를 바꾸는 것이 아무도 없다 — 내 몫(dxi)은 끝에 한 번에
 * 더하고, 이웃 j 는 이 입자를 도는 동안 한 번씩만 만진다. 그러니 두 번째
 * 훑기가 다시 구하는 dx·dy·r 은 첫 번째와 **글자 그대로 같은 값**이다.
 * 첫 훑기에서 담아두고 두 번째는 그것만 본다. 이웃은 입자당 스무 개 남짓이라
 * 담아두는 값이 싸다. sqrtf 가 절반, 격자 훑기도 절반이 된다.
 * 수식은 그대로다 — 시뮬에서 400프레임 지문이 같은지로 확인한다. */
#define NB_MAX 128        /* 한 입자가 보는 이웃 상한. 넘으면 알려준다 */

static void neighbors_relax(float dt)
{
    /* 🚨 스택이 아니라 파일 정적에 둔다. 물리는 코어1 태스크 하나만 돌리므로
     * 안전하고, 태스크 스택을 1.8KB 더 먹지 않는다. */
    static uint16_t nb_j[NB_MAX];
    static float    nb_q[NB_MAX], nb_ux[NB_MAX], nb_uy[NB_MAX];

    float dt2 = dt * dt;
    for (int i = 0; i < NP; i++) {
        float rho = 0, rhon = 0;
        int cx = (int)(s_x[i] / GS), cy = (int)(s_y[i] / GS);
        int nb = 0;

        /* ── 훑기 하나 — 밀도를 세면서 이웃을 담는다 ───────── */
        for (int oy = -1; oy <= 1; oy++) {
            int yy = cy + oy;
            if (yy < 0 || yy >= GC) continue;
            for (int ox = -1; ox <= 1; ox++) {
                int xx = cx + ox;
                if (xx < 0 || xx >= GC) continue;
                for (uint16_t j = s_head[yy * GC + xx]; j != 0xFFFF; j = s_next[j]) {
                    if (j == i) continue;
                    float dx = s_x[j] - s_x[i], dy = s_y[j] - s_y[i];
                    float r2 = dx * dx + dy * dy;
                    if (r2 >= HR2 || r2 < 1e-4f) continue;
                    /* 🚨 나눗셈은 Xtensa 에서 20사이클쯤 한다. HR 은 상수라
                     * 곱셈으로 바꾸고, 1/r 은 한 번만 구해 두 축에 나눠 쓴다. */
                    float inv_r = 1.0f / sqrtf(r2);
                    float r = r2 * inv_r;
                    float q = 1.0f - r * INV_HR;
                    rho  += q * q;
                    rhon += q * q * q;
                    if (nb < NB_MAX) {
                        nb_j[nb]  = j;
                        nb_q[nb]  = q;
                        nb_ux[nb] = dx * inv_r;
                        nb_uy[nb] = dy * inv_r;
                        nb++;
                    } else {
                        /* 🚨 여기 오면 물이 한 자리에 128개 넘게 뭉친 것이다.
                         * 380개 중 3분의 1이 반지름 22 안에 든다는 뜻이라
                         * 실제로는 안 일어난다. 나면 알아야 한다. */
                        static bool told;
                        if (!told) { told = true; ESP_LOGW("water", "이웃이 %d개를 넘었다", NB_MAX); }
                    }
                }
            }
        }

        /* ── 훑기 둘 — 담아둔 것으로 민다 ──────────────────── */
        float P  = KSTIFF * (rho - s_rho0);
        float PN = KNEAR  * rhon;
        float dxi = 0, dyi = 0;
        for (int k = 0; k < nb; k++) {
            float q = nb_q[k];
            float d = dt2 * (P * q + PN * q * q) * 0.5f;
            float ux = nb_ux[k], uy = nb_uy[k];
            uint16_t j = nb_j[k];
            s_x[j] += ux * d;  s_y[j] += uy * d;
            dxi    -= ux * d;  dyi    -= uy * d;
        }
        s_x[i] += dxi; s_y[i] += dyi;
    }
}

static void fluid_step(float dt, float gx, float gy)
{
    /* 점성 — 서로 다가가거나 멀어지는 속도를 조금 나눈다.
     * 이게 있어야 물이 뚝뚝 끊기지 않고 한 덩어리로 흐른다. */
    grid_build_from_current();
    for (int i = 0; i < NP; i++) {
        int cx = (int)(s_x[i] / GS), cy = (int)(s_y[i] / GS);
        for (int oy = -1; oy <= 1; oy++) {
            int yy = cy + oy;
            if (yy < 0 || yy >= GC) continue;
            for (int ox = -1; ox <= 1; ox++) {
                int xx = cx + ox;
                if (xx < 0 || xx >= GC) continue;
                for (uint16_t j = s_head[yy * GC + xx]; j != 0xFFFF; j = s_next[j]) {
                    if (j <= i) continue;             /* 짝마다 한 번만 */
                    float dx = s_x[j] - s_x[i], dy = s_y[j] - s_y[i];
                    float r2 = dx * dx + dy * dy;
                    if (r2 >= HR2 || r2 < 1e-4f) continue;
                    float inv_r = 1.0f / sqrtf(r2);
                    float ux = dx * inv_r, uy = dy * inv_r;
                    float vr = (s_vx[i] - s_vx[j]) * ux + (s_vy[i] - s_vy[j]) * uy;
                    if (vr <= 0) continue;            /* 다가갈 때만 */
                    float q = 1.0f - (r2 * inv_r) * INV_HR;
                    /* 🚨 점성 = 물의 무게감이다. 0.10 이면 입자가 서로를 거의
                     * 안 붙잡아 모래처럼 가볍게 흩어진다(0909 제보: "물 움직임이
                     * 물 같지 않게 너무 가볍다"). 올리면 한 덩어리로 끈적하게
                     * 흐르고, 덤으로 빨리 가라앉아 water_is_still() 이 자주
                     * 걸려서 그리는 값도 준다. */
                    /* 🚨 0.10 → 0.22 → 0.32. 올릴수록 입자가 서로를 붙잡아
                     * 한 덩어리로 끈적하게 흐른다 = 무게감이다(0910 제보:
                     * "중력값을 너무 세게 먹는 것 같다. 무게감을 좀 주자").
                     * 중력만 낮추면 무거운 게 아니라 물속처럼 둥둥 뜬다 —
                     * 점성을 같이 올려야 묵직해진다. */
                    float imp = dt * q * (0.55f * vr) * 0.5f;
                    s_vx[i] -= ux * imp; s_vy[i] -= uy * imp;
                    s_vx[j] += ux * imp; s_vy[j] += uy * imp;
                }
            }
        }
    }

    /* 중력으로 옮긴다 */
    for (int i = 0; i < NP; i++) {
        s_vx[i] += gx * dt;
        s_vy[i] += gy * dt;
        s_px[i] = s_x[i];                 /* 옮기기 전 자리를 기억 */
        s_py[i] = s_y[i];
        s_x[i] += s_vx[i] * dt;
        s_y[i] += s_vy[i] * dt;
    }

    /* 밀도를 고르게 맞춘다 */
    grid_build_from_current();
    neighbors_relax(dt);

    /* 통 안으로 되돌리고, 실제로 움직인 거리에서 속도를 되뽑는다 */
    float inv = 1.0f / dt;
    for (int i = 0; i < NP; i++) {
        clamp_circle(&s_x[i], &s_y[i]);
        s_vx[i] = (s_x[i] - s_px[i]) * inv;
        s_vy[i] = (s_y[i] - s_py[i]) * inv;
        float sp2 = s_vx[i] * s_vx[i] + s_vy[i] * s_vy[i];
        /* 🚨 1000 으로 내렸던 것은 잘못이었다. 이 상한은 **자유낙하 속도보다
         * 높아야 한다** — 낮으면 제대로 떨어지는 물까지 잘라서 오히려 느리고
         * 가벼워 보인다. 통 높이 400 에 중력 1900 이면 √(2·g·h) ≈ 1233 이므로
         * 1300 이 아래 한계다. 이건 폭주를 잡는 그물이지 무게 손잡이가 아니다. */
        if (sp2 > 1300.0f * 1300.0f) {    /* 너무 빠르면 이웃을 뛰어넘어 찢어진다 */
            float k = 1300.0f / sqrtf(sp2);
            s_vx[i] *= k; s_vy[i] *= k;
        }
    }
}

/* 흔든 세기를 부드럽게 매긴다. 문턱을 조금 넘긴 것은 거의 안 먹고, 크게
 * 넘긴 것은 그대로 먹는다 — 문턱에서 값이 뚝 끊기지 않게 0 에서 이어붙인다. */
static float shake_curve(float v)
{
    float a = fabsf(v);
    if (a < 170.0f) return 0.0f;
    a -= 170.0f;
    float t = a / 600.0f;
    if (t > 1.0f) t = 1.0f;
    return (v < 0.0f ? -1.0f : 1.0f) * a * t;
}

/* ── 그리기 ───────────────────────────────────────────────────
 * 입자를 그냥 동그라미로 그리면 알갱이 더미로 보인다. 대신 열마다
 * "여기 물이 어디부터 어디까지 있나"를 세어 이어진 토막으로 칠한다 —
 * 붙어 있으면 한 덩어리로, 떨어져 나간 것은 따로 그려진다. */
#define COLW 6                          /* 세는 열의 너비 */
#define NCOL (466 / COLW + 1)
#define ROWH 6
#define NROW (466 / ROWH + 1)
static uint8_t (*s_dens)[NCOL];
static uint8_t (*s_chur)[NCOL];   /* 그 자리 물이 얼마나 요동치나 — 거품용 */

/* 열마다 물이 있는 토막. 떨어져 나간 덩어리는 따로 잡히게 세 개까지 둔다. */
#define SPANS 3
static int16_t (*s_sp0)[SPANS], (*s_sp1)[SPANS];
static uint8_t *s_spn;
static uint8_t *s_sfrac;    /* 수면이 픽셀의 몇 할을 덮나 — 가장자리를 부드럽게 */
static int8_t  *s_slope;    /* 수면 기울기 — 빛이 닿는 면을 밝게 */
static uint8_t *s_foam;     /* 거품 */
static int      *s_cur;     /* 줄을 훑을 때 열마다 지금 보는 토막 */
static uint16_t *s_scol;    /* 열마다 미리 구해 둔 수면 세 줄 색 */

/* 🚨 물 그림은 우리가 직접 채워 이미지 한 장으로 넘긴다. 왜인지는 draw_cb 위
 * 주석에 있다. 466x466 RGB565 = 434KB — PSRAM 에 3.3MB 가 논다(0909 실측). */
static uint16_t *s_img;
static lv_image_dsc_t s_img_dsc;
/* 지난 프레임에 물이 있던 자리. 거기만 지우면 434KB 를 통째로 안 만진다.
 * 🚨 draw_cb 가 paint_img 보다 앞에 있으므로 선언은 여기 둔다. */
static int s_img_y0 = 0, s_img_y1 = 465;
static lv_image_dsc_t s_img_strip;
static uint16_t s_depth_lut[201];   /* 수면에서 몇 픽셀 깊은가 → 색 */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static void depth_lut_build(void)
{
    for (int d = 0; d <= 200; d++) {
        float t = d / 200.0f;
        s_depth_lut[d] = RGB565((int)(0x6E + (0x0A - 0x6E) * t),
                                (int)(0xC4 + (0x30 - 0xC4) * t),
                                (int)(0xE8 + (0x58 - 0xE8) * t));
    }
}

/* 그 자리의 물 진하기 — 칸 사이는 이어서 읽는다(그래야 안 각진다) */
static float dens_at(float fx, int r)
{
    if (r < 0 || r >= NROW) return 0;
    float c = fx / COLW;
    int   ci = (int)c;
    float t = c - ci;
    if (ci < 0) { ci = 0; t = 0; }
    if (ci >= NCOL - 1) { ci = NCOL - 2; t = 1; }
    return s_dens[r][ci] * (1 - t) + s_dens[r][ci + 1] * t;
}

/* 🚨 6px 칸을 그대로 칠하면 계단처럼 각진다(0909 실측). 진하기가 문턱을
 * 넘는 자리를 칸 사이에서 이어서 찾으면 매끈해진다. 한 프레임에 한 번만
 * 구해두고 밴드마다는 읽기만 한다. */
#define THRESH 3.0f
static void build_spans(void)
{
    memset(s_dens, 0, (size_t)NROW * NCOL);
    memset(s_chur, 0, (size_t)NROW * NCOL);
    for (int i = 0; i < NP; i++) {
        int c = (int)(s_rx[i] / COLW), r = (int)(s_ry[i] / ROWH);
        /* 빠른 입자가 모인 곳이 부서지는 자리다 — 거기에 거품이 인다 */
        float sp = fabsf(s_rvx[i]) + fabsf(s_rvy[i]);
        int ch = (int)(sp * 0.35f);
        if (ch > 60) ch = 60;
        for (int dr = -2; dr <= 2; dr++) {
            int rr = r + dr;
            if (rr < 0 || rr >= NROW) continue;
            for (int dc = -2; dc <= 2; dc++) {
                int cc = c + dc;
                if (cc < 0 || cc >= NCOL) continue;
                int w = 5 - (dr * dr + dc * dc);
                if (w <= 0) continue;
                int v = s_dens[rr][cc] + w;
                s_dens[rr][cc] = v > 255 ? 255 : (uint8_t)v;
                int u = s_chur[rr][cc] + ((ch * w) >> 2);
                s_chur[rr][cc] = u > 255 ? 255 : (uint8_t)u;
            }
        }
    }
    for (int x = 0; x < 466; x++) {
        int n = 0;
        float prev = dens_at(x, 0);
        int   topr = -1; float topf = 0;
        for (int r = 1; r < NROW && n < SPANS; r++) {
            float d = dens_at(x, r);
            bool was = prev >= THRESH, now = d >= THRESH;
            if (!was && now) {                      /* 물이 시작되는 자리 */
                float f = (THRESH - prev) / (d - prev + 1e-6f);
                topr = r - 1; topf = f;
                if (n == 0) {
                    /* 수면이 그 픽셀을 몇 할이나 덮나. 이걸로 맨 윗줄 색을
                     * 섞으면 톱니가 안 보인다(부분 화소). */
                    float yf = (topr + topf) * ROWH;
                    s_sfrac[x] = (uint8_t)((1.0f - (yf - floorf(yf))) * 255.0f);
                }
            } else if (was && !now && topr >= 0) {   /* 끝나는 자리 */
                float f = (prev - THRESH) / (prev - d + 1e-6f);
                s_sp0[x][n] = (int16_t)((topr + topf) * ROWH);
                s_sp1[x][n] = (int16_t)((r - 1 + f) * ROWH);
                if (s_sp1[x][n] > s_sp0[x][n]) n++;
                topr = -1;
            }
            prev = d;
        }
        if (topr >= 0 && n < SPANS) {
            s_sp0[x][n] = (int16_t)((topr + topf) * ROWH);
            s_sp1[x][n] = 465;
            n++;
        }
        s_spn[x] = (uint8_t)n;

        /* 그 자리 물의 요동 — 수면 바로 아래를 본다 */
        int fr = 0;
        if (n) {
            int rr = s_sp0[x][0] / ROWH;
            if (rr >= 0 && rr < NROW) {
                int cc = x / COLW;
                if (cc >= NCOL) cc = NCOL - 1;
                fr = s_chur[rr][cc];
            }
        }
        s_foam[x] = (uint8_t)fr;
    }

    /* 수면 기울기 — 빛을 받는 면을 밝게 하려면 이게 있어야 한다.
     * 평평한 색면만 칠하면 아무리 잘 흘러도 그림이 납작해 보인다. */
    for (int x = 0; x < 466; x++) {
        int a = x > 3 ? x - 4 : 0, b = x < 462 ? x + 4 : 465;
        if (!s_spn[a] || !s_spn[b]) { s_slope[x] = 0; continue; }
        int d = (s_sp0[b][0] - s_sp0[a][0]) * 8 / (b - a);
        if (d > 127) d = 127;
        if (d < -127) d = -127;
        s_slope[x] = (int8_t)d;
    }
}

/* 🚨 왜 물을 우리가 직접 칠하나 — 0909 실기에서 잰 값이 답이다.
 *
 * 예전엔 여기서 열마다 lv_draw_rect 를 불렀다. 깊이 색 10칸 + 수면 3줄이니
 * 열당 13번, 466열이면 6,000번이다. 그런데 실제로는 그보다 훨씬 많았다 —
 * display.c 의 화면 버퍼가 16줄뿐이라(SPI DMA 가 그만큼의 내부 RAM 을
 * 요구하는데 물 앱 돌 때 내부 힙 최저가 23KB 다) LVGL 이 한 프레임을
 * 466/16 = 30조각으로 나눠 그리고, 이 콜백이 조각마다 다시 불려 466열을
 * 처음부터 훑는다. 그래서 한 프레임에 draw_rect 가 2만 번 넘게 불렸다.
 *
 * 잰 값: 계산 39ms · 진짜 프레임 간격 311ms → 초당 3장.
 * 사라진 272ms 가 전부 여기였다. 그리고 그 느린 프레임이 물리의 한 걸음을
 * 33ms 로 키워(8ms 여야 한다) 물을 통째로 위로 날려버렸다.
 *
 * 먼저 세로 그라디언트로 열당 1번으로 줄여봤는데 오히려 느려졌다
 * (311 → 450ms). 열마다 색이 달라 466개의 서로 다른 그라디언트가 생기고,
 * LVGL 은 그라디언트마다 맵을 새로 만들기 때문이다.
 *
 * 그래서 부르는 횟수를 줄이는 대신 아예 안 부르기로 했다. 픽셀은 paint_img()
 * 가 우리 버퍼에 한 번만 찍고(조각 나누기와 무관하다), 여기선 그림 한 장을
 * 넘긴다 — 조각당 1번, 프레임당 30번이다. 2만 번이 30번이 됐다.
 *
 * 🚨 배를 물보다 나중에 그린다. 예전엔 배를 먼저 그리고 물을 덮어 잠긴
 * 부분이 가려졌는데, 그림이 불투명이라 그 순서로는 배가 지워진다. 배가
 * 물에 잠기는 모습이 필요해지면 배도 paint_img() 안에서 찍어야 한다. */
/* 🚨 LVGL 은 무효 영역을 화면 배경(검정)으로 한 벌 칠한 뒤 그 위에 우리
 * 그림을 덮는다. 우리 그림이 그 자리를 불투명하게 다 덮는데도 그렇다 —
 * 같은 픽셀을 두 번 만지는 셈이다. 덮는다고 알려주면 배경칠이 통째로 빠진다
 * (0909: 그리기 75ms 중 한 벌이 여기였다).
 * 🚨 우리가 실제로 칠한 띠(s_img_y0~y1) 안일 때만 '덮는다' 고 해야 한다.
 * 그 밖까지 덮는다고 하면 지워져야 할 자리가 안 지워진다. */
static void cover_cb(lv_event_t *e)
{
    if (!s_img) { lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER); return; }
    const lv_area_t *a = lv_event_get_cover_area(e);
    if (a->y1 >= s_img_y0 && a->y2 <= s_img_y1 && a->x1 >= 0 && a->x2 <= 465)
        lv_event_set_cover_res(e, LV_COVER_RES_COVER);
    else
        lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER);
}

static void draw_cb(lv_event_t *e)
{
    if (!s_x || !s_spn) return;      /* 이미 놓았으면 그리지 않는다 */
    lv_layer_t *layer = lv_event_get_layer(e);

    if (s_img) {
        /* 🚨 물이 있는 띠만 넘긴다. 나머지는 LVGL 이 어차피 배경을 새로
         * 칠하므로 검정으로 남는다 — 넘길 이유가 없다. */
        lv_draw_image_dsc_t idsc;
        lv_draw_image_dsc_init(&idsc);
        /* 띠의 첫 줄을 가리키는 표를 따로 둔다. 그림을 잘라 쓰는 것보다
         * 시작 주소와 높이만 바꿔 주는 쪽이 헷갈릴 여지가 없다. */
        idsc.src = &s_img_strip;
        lv_area_t coords = { 0, s_img_y0, 465, s_img_y1 };
        lv_draw_image(layer, &idsc, &coords);
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    (void)dsc;

}

/* ── 배를 그림에 찍기 ─────────────────────────────────────────
 * 🚨 예전엔 LVGL 삼각형으로 물보다 **먼저** 그려서, 잠긴 부분이 물에 가려졌다.
 * 그림 한 장으로 넘기게 바꾸면서 배가 물보다 뒤로 밀렸고 — 그림이 불투명이라
 * 배가 늘 맨 위에 뜨게 됐다. "물에 절대 안 잠긴다"(0909 제보)가 그것이다.
 * 배도 같은 그림 안에 찍으면 순서가 되살아난다. 덤으로 삼각형 12개 x 조각
 * 20개 = 240번이던 그리기 호출이 사라진다. */
static void fill_tri(float x0, float y0, float x1, float y1,
                     float x2, float y2, uint16_t c)
{
    int ymin = (int)floorf(fminf(y0, fminf(y1, y2)));
    int ymax = (int)ceilf (fmaxf(y0, fmaxf(y1, y2)));
    if (ymin < 0) ymin = 0;
    if (ymax > 465) ymax = 465;
    for (int y = ymin; y <= ymax; y++) {
        float yc = y + 0.5f;
        /* 세 변과 이 가로줄이 만나는 x 를 모은다 */
        float xs[3]; int nx = 0;
        float px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
        for (int e = 0; e < 3; e++) {
            int f = (e + 1) % 3;
            float ya = py[e], yb = py[f];
            if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya)) {
                float t = (yc - ya) / (yb - ya);
                xs[nx++] = px[e] + (px[f] - px[e]) * t;
            }
        }
        if (nx < 2) continue;
        float xa = xs[0], xb = xs[1];
        if (xa > xb) { float t = xa; xa = xb; xb = t; }
        int ia = (int)ceilf(xa - 0.5f), ib = (int)floorf(xb - 0.5f);
        if (ia < 0) ia = 0;
        if (ib > 465) ib = 465;
        uint16_t *row = s_img + (size_t)y * 466;
        for (int x = ia; x <= ib; x++) row[x] = c;
    }
}

/* 지금 자세의 배를 그림에 찍고, 차지한 세로 범위를 돌려준다. */
static void paint_boat(int *out_y0, int *out_y1)
{
    float bx = s_boat_x, by = s_boat_y;
    float bdeg = 0;
    {   /* 배 밑 물살의 기울기로 눕힌다 */
        float lft = 0, rgt = 0; int nl = 0, nr = 0;
        for (int i = 0; i < NP; i++) {
            float d = s_rx[i] - bx;
            if (fabsf(d) > 40 || s_ry[i] > by + 40) continue;
            if (d < 0) { lft += s_ry[i]; nl++; } else { rgt += s_ry[i]; nr++; }
        }
        if (nl && nr) bdeg = atanf(((rgt / nr) - (lft / nl)) / 40.0f) / DEG2RAD * 0.6f;
    }
    /* 🚨 중력 쪽으로 세운다. 부호를 뒤집어 넣었다가 90도·270도 에서만 배가
     * 거꾸로 서서 돛이 물에 닿았다(0909 제보) — 0도·180도 는 부호를 뒤집어도
     * 같은 각이라 절반만 틀린 꼴로 보인다. */
    bdeg -= atan2f(s_gux, s_guy) / DEG2RAD;

    /* 🚨 여태 이 각을 그대로 썼다 — 기울이는 즉시 배가 홱 돌아섰다. 무거운
     * 배는 그렇게 못 돈다. 늦게 따라가게 한다.
     * 🚨 각은 ±180 에서 넘어가므로 차이를 먼저 그 안으로 접어야 한다.
     * 안 접으면 한 바퀴를 거꾸로 도는 꼴이 난다. */
    {
        static float sm;
        static bool  first = true;
        if (first) { sm = bdeg; first = false; }
        float d = bdeg - sm;
        while (d >  180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        sm += d * 0.11f;
        bdeg = sm;
    }
    float bc = cosf(bdeg * DEG2RAD), bs = sinf(bdeg * DEG2RAD);

    int lo = 465, hi = 0;
    #define BX(px, py) (bx + (px) * bc - (py) * bs)
    #define BY(px, py) (by + (px) * bs + (py) * bc)
    #define TRI(c, ax, ay, bx_, by_, cx, cy) do {         float _y0 = BY(ax, ay), _y1 = BY(bx_, by_), _y2 = BY(cx, cy);         fill_tri(BX(ax, ay), _y0, BX(bx_, by_), _y1, BX(cx, cy), _y2, (c));         float _lo = fminf(_y0, fminf(_y1, _y2)), _hi = fmaxf(_y0, fmaxf(_y1, _y2));         if ((int)_lo < lo) lo = (int)_lo;         if ((int)_hi + 1 > hi) hi = (int)_hi + 1;     } while (0)
    /* 🚨 어두운 실루엣으로 갔더니 짙은 물에 묻혀 안 보였다(0909).
     * 선체를 나무빛으로 밝히고 테두리를 한 톤 더 올린다. */
    const uint16_t HULL  = RGB565(0xB0, 0x70, 0x3C), HULL2 = RGB565(0xE0, 0xA4, 0x68);
    const uint16_t SAIL  = RGB565(0xFF, 0xFD, 0xF6), SAIL2 = RGB565(0xE8, 0xDC, 0xC4);
    const uint16_t FLAG  = RGB565(0xFF, 0xE0, 0x8A);
    TRI(HULL,  -30,-7,  -14,-11, -16, 6);
    TRI(HULL,  -14,-11,  10,-11, -16, 6);
    TRI(HULL,   10,-11,  14,  6, -16, 6);
    TRI(HULL,   10,-11,  33, -9,  14, 6);
    TRI(HULL2, -30,-7,   33, -9, -14,-11);
    TRI(HULL2,  -2,-52,   1,-52,   3,-11);
    TRI(HULL2,   1,-52,   3,-11,   0,-11);
    TRI(SAIL,    3,-50,  22,-24,   3,-13);
    TRI(SAIL,   22,-24,  26,-15,   3,-13);
    TRI(SAIL2,   3,-50,   9,-31,   3,-13);
    TRI(SAIL2,  -2,-46, -19,-13,  -2,-13);
    TRI(FLAG,   -6,-14,  -3,-14,  -4,-18);
    #undef TRI
    #undef BY
    #undef BX
    if (lo < 0) lo = 0;
    if (hi > 465) hi = 465;
    *out_y0 = lo;
    *out_y1 = hi;
}

/* ── 물 그림 채우기 ───────────────────────────────────────────
 * 토막(s_sp0/s_sp1)은 step() 이 이미 구해뒀다. 여기선 픽셀만 찍는다.
 * 조각 나누기와 무관하게 한 번만 돈다 — 그게 이 방식의 요점이다. */
static void paint_img(void)
{
    if (!s_img) return;
    /* 🚨 매 프레임 434KB 를 memset 하고 466x466 을 통째로 넘기는 건 낭비다.
     * 물은 대개 아래쪽 절반에만 있다. 지난번에 그린 만큼만 지우고, 이번에
     * 그린 만큼만 넘긴다(0909: 그리기 78ms 중 상당수가 안 쓰는 자리였다). */
    memset(s_img + (size_t)s_img_y0 * 466, 0,
           (size_t)(s_img_y1 - s_img_y0 + 1) * 466 * 2);
    int ny0 = 465, ny1 = 0;

    /* 🚨 배를 물보다 **먼저** 찍는다. 그래야 잠긴 부분을 물이 덮는다.
     * 순서가 뒤집혔던 탓에 배가 늘 맨 위에 떠 있었다(0909 제보). */
    {
        int b0, b1;
        paint_boat(&b0, &b1);
        if (b0 < ny0) ny0 = b0;
        if (b1 > ny1) ny1 = b1;
    }
    /* 🚨 예전엔 열을 먼저 돌고 그 안에서 세로로 내려갔다. 그림 버퍼는 가로로
     * 누워 있는데(한 줄이 932바이트) 세로로 훑으면 픽셀 하나 쓸 때마다 주소가
     * 932바이트씩 점프한다 — 21만 번이 전부 캐시 라인이 다르다. PSRAM 에
     * 그러니 그림 채우기에만 35ms 가 들었다(0909 실측).
     * 가로로 뒤집어 한 줄을 이어서 쓴다. 쓰기가 버스트로 나간다.
     *
     * 열마다 필요한 것(수면 세 줄 색, 깊이 기준)은 미리 구해 둔다 — 줄을 도는
     * 안쪽 고리는 되도록 가볍게. */
    int wy0 = 465, wy1 = 0;
    for (int x = 0; x < 466; x++) {
        s_cur[x] = 0;
        int nsp = s_spn[x];
        if (!nsp) continue;
        int s0 = s_sp0[x][0];
        if (s0 < wy0) wy0 = s0;
        int last = s_sp1[x][nsp - 1];
        if (last > wy1) wy1 = last;

        /* ── 수면 세 줄 ─────────────────────────────────────
         * 🚨 평평한 색면만 칠하면 아무리 잘 흘러도 납작해 보인다.
         * 물처럼 보이게 하는 건 세 가지다:
         *   기울기  빛을 향한 면은 밝고 등진 면은 어둡다
         *   거품    부서지는 자리는 하얗다
         *   덮임    맨 윗줄을 덮인 만큼만 칠해 톱니를 없앤다 */
        int sl = s_slope[x];                 /* 오른쪽으로 내려가면 +  */
        int lit = 128 - sl * 2;              /* 빛은 왼쪽 위에서 */
        if (lit < 40) lit = 40;
        if (lit > 255) lit = 255;
        int fo = s_foam[x];
        for (int q = 0; q < 3; q++) {
            int br = (0x9A * lit) >> 8, bg = (0xD6 * lit) >> 8, bb2 = (0xF2 * lit) >> 8;
            if (q) { br = br * 3 / 4; bg = bg * 3 / 4; bb2 = bb2 * 3 / 4; }
            if (fo) {
                int f = fo > 200 ? 200 : fo;
                br += (255 - br) * f / 255;
                bg += (255 - bg) * f / 255;
                bb2 += (255 - bb2) * f / 255;
            }
            if (q == 0) {
                int cov = s_sfrac[x];
                br = br * cov / 255; bg = bg * cov / 255; bb2 = bb2 * cov / 255;
            }
            if (br > 255) br = 255;
            if (bg > 255) bg = 255;
            if (bb2 > 255) bb2 = 255;
            s_scol[x * 3 + q] = RGB565(br, bg, bb2);
        }
    }
    if (wy0 < 0) wy0 = 0;
    if (wy1 > 465) wy1 = 465;

    /* 🚨 어떻게 훑느냐가 값을 정한다. 두 번 틀리고 세 번째에 맞췄다(0909).
     *
     *   1) 열을 먼저 돌고 세로로 내려가기 — 픽셀마다 932바이트씩 점프한다.
     *      한 열을 내려가며 200줄을 만지면 200개의 캐시 라인을 건드리는데,
     *      그 작업 크기가 186KB 라 64KB 캐시에 안 들어간다. 35ms.
     *   2) 줄을 먼저 돌고 가로로 훑기 — 쓰기는 이어지지만 물 없는 열까지
     *      전부 봐야 해서 훑는 횟수가 8만 7천 → 21만 번이 됐다. 65ms. 더 나빴다.
     *   3) **세로로 훑되 64줄씩 끊기** — 띠 하나의 작업 크기가
     *      64 x 932 = 59KB 라 캐시 안에 들어간다. 물 있는 픽셀만 만지면서
     *      캐시도 안 어긋난다. 둘의 좋은 쪽만 남는다. */
    #define BANDH 64
    for (int b0 = wy0; b0 <= wy1; b0 += BANDH) {
        int b1 = b0 + BANDH - 1;
        if (b1 > wy1) b1 = wy1;
        for (int x = 0; x < 466; x++) {
            int nsp = s_spn[x];
            if (!nsp) continue;
            /* 🚨 깊이는 "이 픽셀 위에 물이 얼마나 있나" 로 잰다.
             * 처음엔 그 토막의 머리에서 쟀더니 토막마다 0부터 다시 세어
             * 토막 머리마다 밝은 줄이 생겼다. 그래서 '이 열의 첫 토막 머리'
             * 로 바꿨더니 이번엔 세로줄이 생겼다(0909 제보) — 물방울 하나가
             * 위로 튀어 있으면 그 아래 전체가 '아주 깊은 물' 이 되는데 옆
             * 열은 안 그러니 경계가 선다.
             * 위에 실제로 쌓인 두께를 세면 둘 다 없어진다. 떠 있는 물방울은
             * 제 두께만큼만 보태므로 아래 물빛을 안 흔든다. */
            int above = 0;
            for (int k = 0; k < nsp; k++) {
                int t0 = s_sp0[x][k], t1 = s_sp1[x][k];
                int a0 = t0 < b0 ? b0 : t0;
                int a1 = t1 > b1 ? b1 : t1;
                if (a1 >= a0) {
                    for (int y = a0; y <= a1; y++) {
                        int q = above + (y - t0);
                        s_img[(size_t)y * 466 + x] =
                            (k == 0 && q < 3) ? s_scol[x * 3 + q]
                                              : s_depth_lut[q > 200 ? 200 : q];
                    }
                }
                above += t1 - t0 + 1;
            }
        }
    }
    #undef BANDH
    if (wy1 >= wy0) {
        if (wy0 < ny0) ny0 = wy0;
        if (wy1 > ny1) ny1 = wy1;
    }
    if (ny1 < ny0) { ny0 = 0; ny1 = 0; }      /* 물이 하나도 없었다 */
    s_img_y0 = ny0;
    s_img_y1 = ny1;
    s_img_strip = s_img_dsc;
    s_img_strip.header.h  = (uint32_t)(ny1 - ny0 + 1);
    s_img_strip.data      = (const uint8_t *)(s_img + (size_t)ny0 * 466);
    s_img_strip.data_size = (size_t)(ny1 - ny0 + 1) * 466 * 2;
}

/* ── 한 프레임 ─────────────────────────────────────────────── */
/* 🔋 물이 잔잔하고 손도 안 대면 계산할 이유가 없다.
 * 입자 620개를 굴리는 건 이 기기에서 제일 비싼 축이라, 멈춘 물을 계속
 * 푸는 건 그냥 배터리를 태우는 짓이다. 다 가라앉으면 쉬고, 조금이라도
 * 움직이거나 손이 닿으면 곧바로 깨어난다. */
static bool water_is_still(void)
{
    float e = 0;
    for (int i = 0; i < NP; i += 4)          /* 넷에 하나만 봐도 충분하다 */
        e += fabsf(s_vx[i]) + fabsf(s_vy[i]);
    return e < (NP / 4) * 1.2f;
}


/* ── 프레임 시간 눈금 ─────────────────────────────────────────
 * 🔋 전력을 재려면 한 프레임에 CPU 를 얼마나 쓰는지부터 알아야 한다.
 * 128번 모아 한 줄만 찍는다 — 로그 자체가 부담이 되면 안 되니까. */
static void frame_tick(const char *who, int64_t t0)
{
    static uint32_t n; static uint64_t sum; static uint32_t hi; static int64_t since;
    /* 🚨 계산하는 시간만 재면 반쪽이다. 그 뒤에 LVGL 이 실제로 칠하는 일과
     * 화면으로 밀어 보내는 일이 빠진다 — 물은 build_spans() 가 데이터만
     * 준비하고 진짜 칠하기는 나중에 일어나서, "초당 25장" 이라 해놓고 눈에는
     * 그대로였다(0909 지적). 이 함수가 다시 불릴 때까지의 **진짜 간격**을
     * 같이 잰다. 그게 사람이 보는 속도다. */
    static int64_t prev; static uint64_t gap; static uint32_t gapn; static uint32_t gaphi;
    int64_t now = esp_timer_get_time();
    uint32_t us = (uint32_t)(now - t0);
    sum += us; if (us > hi) hi = us;
    if (prev && now - prev < 2000000) {          /* 앱을 새로 연 참이면 건너뛴다 */
        uint32_t g = (uint32_t)(now - prev);
        gap += g; gapn++; if (g > gaphi) gaphi = g;
    }
    prev = now;
    if (!since) since = now;
    /* 🚨 예전엔 128프레임마다 찍었는데, 물처럼 한 프레임이 오래 걸리는 쪽은
     * 그 수를 못 채워 아무것도 안 나왔다. 느릴수록 알아야 하는데 느릴수록
     * 입을 다무는 눈금이었다. 이제 3초마다 찍는다. */
    if (++n && now - since >= 3000000) {
        since = now;
        ESP_LOGI(who, "계산 %u us(최대 %u) · 진짜 간격 %u us → 실제 초당 %u장  [%u번]",
                 (unsigned)(sum / n), (unsigned)hi,
                 (unsigned)(gapn ? gap / gapn : 0),
                 (unsigned)(gap && gapn ? 1000000ULL * gapn / gap : 0),
                 (unsigned)n);
        n = 0; sum = 0; hi = 0; gap = 0; gapn = 0; gaphi = 0;
    }
}

static void step(lv_timer_t *t)
{
    /* 🔋 화면이 꺼지면 그릴 이유가 없다. 🚨 여기서 주기를 바꾸면 안 된다 —
     * lv_timer_set_period() 가 안쪽에서 lv_timer_handler_resume() 을 불러서
     * 타이머 콜백에서 부르면 처리기가 그 자리에서 무한히 다시 돈다.
     * 주기는 그대로 두고 60번에 한 번만 일한다. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 60) return;
    }

    int64_t _t0 = esp_timer_get_time();
    uint32_t now = lv_tick_get();
    float dt = s_last_ms ? (now - s_last_ms) / 1000.0f : 0.033f;
    s_last_ms = now;
    if (dt > 0.10f) dt = 0.10f;

    /* ── 중력 ───────────────────────────────────────────────
     * 🚨 IMU 축은 화면 축과 90도 돌아가 있다 — gx 가 세로(앞뒤),
     * gy 가 가로(좌우)를 맡는다. 구슬에서 확인한 그대로다.
     *
     * 🚨 예전엔 '어느 쪽으로 기울었나' 만 보고 세기는 1400 으로 박아뒀다.
     * 그러면 위아래로 흔들어도 물이 아무 반응을 안 한다 — 흔들기는 방향이
     * 아니라 '세기' 가 변하는 일이라서다(0909 지적).
     * 이제 세 축을 그대로 받아 실제 세기를 쓴다. 아래로 흔들면 물이
     * 무거워져 눌리고, 위로 채면 가벼워져 떠오른다. 화면 정면(Z) 으로
     * 흔들어도 크기가 변하니 그것까지 잡힌다. */
    float ax, ay, az = 1000.0f;
    bool have = port_imu_accel3(&ax, &ay, &az);
    if (!have) { ax = s_fake_gx; ay = s_fake_gy; az = 1000.0f; }

    /* ── 기울이는 것도 조작이다 ─────────────────────────────
     * 🚨 런처는 `lv_display_get_inactive_time()` 으로 무동작을 재는데 그건
     * **터치만** 조작으로 친다. 물은 배를 기울여 몰기 때문에 화면을 안
     * 만지고, 그러면 한창 갖고 노는 중에 30초 만에 꺼진다(0909 제보).
     * 기울기 게임에서 겪은 것과 같은 문제이고 같은 방식으로 푼다.
     *
     * 자세가 실제로 바뀌거나 흔들 때만 깨워둔다 — 책상에 내려놓으면
     * 안 움직이니 평소처럼 꺼진다. 🚨 물이 제 힘으로 출렁이는 것은
     * 조작이 아니다. 그래서 물결이 아니라 **IMU 값**을 본다. */
    if (have) {
        static float wx, wy, wz; static bool wset;
        if (!wset) { wx = ax; wy = ay; wz = az; wset = true; }
        float d = fabsf(ax - wx) + fabsf(ay - wy) + fabsf(az - wz);
        if (d > 45.0f) {              /* 손떨림은 조작이 아니다 */
            wx = ax; wy = ay; wz = az;
            lv_display_trigger_activity(NULL);
        }
    }

    /* 🚨 예전엔 앱을 열 때의 자세를 '수평' 으로 박아두고(s_g0x/s_g0y) 거기서의
     * 기울기만 봤다. 게다가 각도를 ±60도로 잘랐다. 그래서 어떻게 돌리든 물이
     * '들고 있던 자세 기준의 아래' 로만 갔고, 360도 회전은 표현할 방법 자체가
     * 없었다(0909 제보: "어느 방향으로 돌리든 그 방향 아래로 물이 가야 한다").
     *
     * 이제 진짜 중력을 그대로 쓴다. 축은 실기에서 재서 정했다 — 배지를 세워
     * 들었을 때 ax=-960 ay=-50 az=+120 이었다. 가속도계는 하늘을 향한 축이
     * 양수이므로(눕히면 az=+1000) -x 가 화면 위, 곧 +x 가 화면 아래다.
     * 중력은 읽은 값의 반대이니:
     *     화면 오른쪽 = -ay      화면 아래 = -ax
     * 🚨 이 매핑은 흔들기 코드(kick_x=-jy, kick_y=-jx)와도 맞아떨어진다.
     * 서로 독립적인 두 근거가 같은 답을 가리켰다. */
    float dirx = -ay, diry = -ax;
    float dlen = sqrtf(dirx * dirx + diry * diry);
    /* 🚨 눕혀 두면(화면이 하늘을 봄) 화면 평면에 중력이 거의 안 비친다.
     * 그때 방향을 억지로 뽑으면 잡음으로 빙빙 돈다 — 그냥 아래로 둔다. */
    if (dlen < 120.0f) { dirx = 0.0f; diry = 1.0f; }
    else               { dirx /= dlen; diry /= dlen; }

    /* 세기 — 가만히 들고 있으면 1g 라 1.0 이다. 흔들면 위아래로 출렁인다.
     * 너무 크면 입자가 이웃을 뛰어넘어 찢어지므로 위아래를 막는다. */
    float mag = 1.0f;
    if (have) {
        mag = sqrtf(ax * ax + ay * ay + az * az) / 1000.0f;
        if (mag < 0.25f) mag = 0.25f;
        if (mag > 2.6f)  mag = 2.6f;
    }

    /* ── 어느 방향으로 흔들든 잡는다 ─────────────────────────
     * 🚨 세기만 쓰면 화면 정면(Z) 으로 찌를 때 물이 눌리기만 하고 안 튄다.
     * 방향이 없는 힘이라 그렇다. 축마다 '갑자기 변한 양'을 따로 재서
     * 그만큼 물을 민다:
     *   가로·세로(x,y) 로 채면 → 그 반대로 쏠린다 (통을 옆으로 친 것)
     *   정면(z) 으로 찌르면    → 방향이 없으니 사방으로 튄다
     * 통을 실제로 흔들 때 물이 벽을 때리는 게 이 힘이다. */
    static float px_, py_, pz_;
    static bool  pfirst = true;
    float jx = 0, jy = 0, jz = 0;
    if (have) {
        if (pfirst) { px_ = ax; py_ = ay; pz_ = az; pfirst = false; }
        jx = ax - px_; jy = ay - py_; jz = az - pz_;
        /* 🚨 문턱만 두고 넘긴 양을 곧바로 비례해 먹이면, 문턱을 아주 조금
         * 넘긴 작은 움직임도 그 양만큼 그대로 들어간다 — "살짝만 움직여도
         * 물이 너무 거세게" 가 그것이다(0910 제보).
         * 넘긴 양을 제곱꼴로 준다: 조금 넘으면 거의 안 먹고, 크게 넘으면
         * 그대로 먹는다. 문턱에서 값이 뚝 끊기지도 않는다(0 에서 이어진다). */
        jx = shake_curve(jx); jy = shake_curve(jy); jz = shake_curve(jz);
        px_ = ax; py_ = ay; pz_ = az;
    }
    /* 축 대응은 구슬과 같다 — x 가 화면 세로, y 가 화면 가로 */
    /* 🚨 흔들면 확 흩어져서 "먼지" 로 보였다(0910). 세기를 절반으로 줄인다 —
     * 무거운 것은 같은 힘으로 흔들어도 덜 날아간다. */
    float kick_x = -jy * 1.1f, kick_y = -jx * 1.1f;
    float burst  = fabsf(jz) * 1.3f;      /* 정면으로 찌른 힘 */
    /* 🚨 중력을 1400 → 1100 으로 낮췄던 것을 되돌린다. 그건 틀린 처방이었다 —
     * **중력을 낮추면 무거워지는 게 아니라 달에 간 것처럼 둥둥 뜬다.** 먼지로
     * 보인 데엔 그 몫이 크다.
     * 무게감은 세 가지에서 온다: (1) 떨어질 땐 제대로 빨리 떨어질 것,
     * (2) 흔들어도 덜 날아갈 것(위의 세기), (3) 어느 쪽이 아래인지 늦게 알
     * 것(아래의 관성). 중력은 (1) 담당이라 오히려 올려야 한다. */
    const float G = 1900.0f;
    float want_x = G * mag * dirx;
    float want_y = G * mag * diry;
    /* 방향은 조금 늦게 따라가되(물의 관성), 세기는 곧바로 먹인다 —
     * 흔드는 건 짧은 순간이라 늦추면 아예 안 느껴진다. */
    s_gx += (want_x - s_gx) * 12.0f * dt;
    s_gy += (want_y - s_gy) * 12.0f * dt;
    if (have) {
        /* 🚨 0.55 는 사실상 즉시였다 — 기울이는 순간 물이 어느 쪽이 아래인지
         * 곧바로 알아버려 통째로 쏠렸다. 그게 가벼워 보이던 큰 이유다.
         * 물은 무거워서 늦게 안다. 0.22 로 낮춘다. */
        s_gx = s_gx * 0.86f + want_x * 0.14f;
        s_gy = s_gy * 0.86f + want_y * 0.14f;
    }

    /* 배와 그리기가 같은 '아래' 를 쓰게 남겨둔다. */
    {
        float gl = sqrtf(s_gx * s_gx + s_gy * s_gy);
        if (gl > 1.0f) { s_gux = s_gx / gl; s_guy = s_gy / gl; }
    }

    /* 🚨 PBF 는 힘이 아니라 위치를 고치는 방식이라 잘 안 터지지만, 한 걸음이
     * 너무 길면 입자가 이웃을 뛰어넘어 물이 찢어진다. 8ms 로 쪼갠다.
     *
     * 🚨 걸음 수를 위에서 묶는다. 안 묶으면 느려질수록 dt 가 커지고, dt 가
     * 커지면 걸음이 늘어 더 느려진다 — 죽음의 나선이다(0909: 초당 8장에서
     * 한 프레임에 걸음 6개, 물리에만 106ms). */
    /* 🚨 예전엔 걸음을 2로 묶었다. 물리가 코어0 에서 그리기와 줄 서 있었기
     * 때문인데, 이제 코어1 로 갔으니 그 이유가 없어졌다. 묶어두면 프레임
     * 하나에 20ms 어치만 계산해서 물이 4분의 1 속도로 흐른다 — 튀어오르는
     * 건 순간이라 티가 덜 나는데, 가라앉고 뭉치는 건 시간이 걸리는 일이라
     * 느린 게 그대로 보인다(0909 제보: "다시 뭉치고 안정화 되는 속도가
     * 실제 물리랑 다른 것 같아"). 실제 흐른 시간만큼 계산한다. */
    int sub = (int)(dt / 0.010f) + 1;
    /* 🚨 코어1 이 한 프레임 안에 못 끝내면 그리기가 그걸 기다리느라 프레임이
     * 통째로 무너진다. 걸음 하나가 얼마나 걸리는지는 입자 수·물 상태에 따라
     * 달라지므로 숫자를 박아두면 안 된다 — 지난 판이 실제로 걸린 시간을 보고
     * 스스로 정한다.
     * 🚨 처음엔 60% 로 잡았는데 너무 짰다. 코어0 이 어차피 프레임의 대부분을
     * 쓰므로(0909: 80ms 중 79ms) 코어1 도 그만큼 써도 서로 안 기다린다.
     * 90% 로 올린다 — 남은 10% 는 넘기기와 흔들림 때문에 둔다. */
    if (s_phys_us && s_phys_sub > 0) {
        float per = (float)s_phys_us / (float)s_phys_sub / 1000000.0f;
        int room = (int)(dt * 0.9f / (per > 1e-6f ? per : 1e-6f));
        if (room < 1) room = 1;
        if (sub > room) sub = room;
    } else if (sub > 2) {
        sub = 2;                  /* 아직 재본 적 없다 — 조심해서 시작한다 */
    }
    float sdt = dt / sub;
    /* 🚨 걸음 수만 묶으면 프레임이 느려질 때 한 걸음이 그만큼 굵어진다.
     * 311ms 프레임에서 한 걸음이 33ms 였고, PBF 는 '움직인 거리 / dt' 로
     * 속도를 되뽑기 때문에 되뽑힌 속도가 폭주해 물이 통째로 날아갔다.
     * 걸음 길이를 막는다 — 느리면 물이 느려질 뿐 터지지는 않는다. */
    if (sdt > 0.010f) sdt = 0.010f;

    /* ── 물리를 코어1 에 넘긴다 ───────────────────────────
     * 🚨 순서가 중요하다. 지난 판이 끝난 것을 보고 → 사본을 뜨고 → 다음 판을
     * 건다. 사본을 뜨는 동안에는 아무도 배열을 안 건드린다. */
    phys_wait();
#ifndef BADGE_SIM
    memcpy(s_rx,  s_x,  NP * sizeof(float));
    memcpy(s_ry,  s_y,  NP * sizeof(float));
    memcpy(s_rvx, s_vx, NP * sizeof(float));
    memcpy(s_rvy, s_vy, NP * sizeof(float));
#endif
    s_pin.gx = s_gx; s_pin.gy = s_gy;
    s_pin.kx = kick_x; s_pin.ky = kick_y; s_pin.burst = burst;
    s_pin.sdt = sdt;   s_pin.sub = sub;
#ifdef BADGE_SIM
    phys_round();                 /* 시뮬은 코어가 하나뿐이다 */
#else
    s_inflight = true;
    xSemaphoreGive(s_go);
#endif


    /* ── 배 ─────────────────────────────────────────────────
     * 주변 입자가 밀어 올리고 실어 나른다. 물이 배를 덮치면 잠긴다. */
    /* 🚨 예전엔 화면 세로축이 곧 '아래' 였다. 배지를 돌리면 배가 옆으로
     * 누운 채 엉뚱한 쪽으로 떠올랐다. 중력이 아래인 틀로 바꿔서 같은 계산을
     * 그대로 한다 — 중력이 화면 아래를 향하면 예전과 똑같이 돈다. */
    #define BOAT_G 1900.0f               /* 물의 중력과 같게 */
/* 🚨 선체는 원점 기준 위로 11px(갑판) 아래로 6px(용골)다(paint_boat 좌표).
 * 평형이 원점 9px 아래면 갑판이 수면과 같은 높이가 돼 **돛만 보인다**
 * (0910 제보). 기준선을 올려 원점이 수면보다 3px 위에서 뜨게 한다 —
 * 용골은 3px 잠기고 갑판은 14px 뜬다. 평형 잠김(BOAT_G/K)이 9 이므로
 * 기준선을 12 올리면 bA-top = 9-12 = -3 이 된다. */
#define BOAT_LIFT   12.0f
/* 🚨 다 잠기면 뜨는 힘은 더 안 는다(잠긴 부피가 곧 힘인데 부피가 다 찼다).
 * 이걸 안 막으면 **물이 배 위를 덮칠 때 로켓이 된다** — top 은 그 띠에서
 * 제일 높은 알갱이라, 물보라가 배 위로 튀면 top 이 확 올라가고 depth 가
 * 폭발한다(0910 제보). 선체가 다 잠기는 깊이에서 끊는다. */
#define BOAT_SUB_MAX 26.0f
    float bC = G_CROSS(s_boat_x, s_boat_y), bA = G_ALONG(s_boat_x, s_boat_y);
    float vC = s_boat_vx * s_guy - s_boat_vy * s_gux;
    float vA = s_boat_vx * s_gux + s_boat_vy * s_guy;

    float svc = 0; int n = 0;
    float top = 1e9f;
    /* 🚨 배 밑 물이 기울어 있으면 배는 그 비탈을 타고 내려가야 한다. 여태
     * 안 그랬던 이유: 뜨는 힘을 **중력 축으로만** 줬다. 그러면 수면이 아무리
     * 기울어도 옆으로 미는 힘이 아예 안 생긴다 — 배가 비탈 위에 붙어 있었다
     * (0910 제보). 진짜 부력은 수면에 수직이라, 수면이 θ 만큼 기울면
     * 중력과 합쳐져 비탈 방향으로 g·sinθ 가 남는다.
     * 그래서 좌우 수면 높이를 따로 재서 기울기를 뽑는다. */
    float tl = 1e9f, tr = 1e9f;
    for (int i = 0; i < NP; i++) {
        float c = G_CROSS(s_rx[i], s_ry[i]) - bC;
        if (fabsf(c) > 34) continue;
        svc += s_rvx[i] * s_guy - s_rvy[i] * s_gux;
        n++;
        float a = G_ALONG(s_rx[i], s_ry[i]);
        if (a < top) top = a;            /* 중력 반대쪽으로 제일 높은 물 */
        if (c < 0) { if (a < tl) tl = a; }
        else       { if (a < tr) tr = a; }
    }
    /* 오른쪽이 낮으면(a 가 크면) 양수 — 그쪽으로 미끄러진다. */
    float slope = 0.0f;
    if (tl < 1e8f && tr < 1e8f) {
        slope = (tr - tl) / 34.0f;
        /* 🚨 물보라 한 알갱이가 한쪽에만 튀면 기울기가 터무니없이 커진다.
         * sinθ 는 원래 1을 못 넘는 값이라 그 언저리에서 끊는다. */
        if (slope >  0.6f) slope =  0.6f;
        if (slope < -0.6f) slope = -0.6f;
    }
    /* 🚨 여태 수면을 향한 스프링 하나로 풀었다. 그게 "공처럼 튄다" 의 정체다 —
     * **물 밖에서도 수면이 배를 끌어당긴다.** 높은 데서 떨어지면 거리에 비례해
     * 끌려 내려와 실제보다 세게 꽂히고, 나올 때도 같은 세기로 튕겨 나간다.
     * 물 안과 밖을 가른다:
     *     밖 — 그냥 떨어진다 (수면은 아무 힘도 안 준다)
     *     안 — 잠긴 깊이만큼 뜨고, 깊을수록 물이 세게 붙잡는다
     * 그러면 첨벙 들어갔다가 묵직하게 올라온다. */
    /* 🚨 배만 잘게 나눠 푼다. 물 앱은 초당 12장인데, 얕게 뜨게 하려고 부력을
     * 세게 잡으면 출렁이는 주기가 0.43초까지 짧아진다 — 한 번의 출렁임이
     * 다섯 프레임이다. 그 해상도에서는 적분기가 진동을 진동으로 못 보고
     * 뭉개버려서 **통통 튀는 것이 수치적으로 사라진다.** 배는 스칼라 몇
     * 개라 잘게 푸는 값이 거의 공짜다. 물살 정보(top·svc·n)는 이 프레임의
     * 것으로 고정해 두고 배만 여러 걸음 걷는다. */
    int bsub = (int)(dt / 0.02f) + 1;
    if (bsub > 8) bsub = 8;
    float bdt = dt / (float)bsub;

    for (int bs_i = 0; bs_i < bsub; bs_i++) {
        /* 기준선을 올린 만큼 더해 준다 — 이 값이 0 이면 '떠 있는 자리' 다. */
        float depth = bA - top + BOAT_LIFT;
        if (depth > BOAT_SUB_MAX) depth = BOAT_SUB_MAX;   /* 다 잠겼다 */
        if (n && depth > 0.0f) {
            /* 🚨 잠긴 깊이에 비례해 뜬다(아르키메데스). 굳기 K 는 흘수가
             * 정한다 — 가라앉은 깊이에서 무게와 균형이 잡히므로
             * 흘수 = BOAT_G/K. 9px 를 노려 210(전 76 은 25px 라 잠겨 보였다). */
            const float K = 210.0f;
            /* 🚨 감쇠는 "몇 번 통통거리다 잦아드나" 를 정한다. 얕을 땐 약하게
             * 둬야 물 밖으로 살짝 튀어올랐다 떨어지기를 되풀이한다(감쇠비
             * 0.14 면 한 번 튈 때마다 높이가 3분의 1로 준다). */
            float f = depth / BOAT_SUB_MAX;
            float C = 4.0f + 6.0f * f;
            /* 🚨 들어갈 때와 나올 때의 저항을 다르게 준다. 같으면 첫 다이빙이
             * 얕아진다(0910 제보: "처음 떨어졌을 때 더 깊이 잠겨야"). 실제로도
             * 물에 꽂히는 선체는 공기를 끌고 들어가 덜 막히고, 떠오를 때는
             * 물기둥을 통째로 밀어내며 올라온다. */
            if (vA > 0.0f) C *= 0.55f;   /* 내려가는 중 */
            /* 🚨 `vA += (k*오차 - c*vA)*dt` 는 명시적이라 `k*dt` 가 2를 넘으면
             * 발산한다. 잠긴 형태로 푼다 — 분모가 늘 1보다 커서 안 터진다. */
            float denom = 1.0f + C * bdt + K * bdt * bdt;
            vA = (vA + (BOAT_G - K * depth) * bdt) / denom;
            /* 🚨 예전엔 여기서 떠오르는 속도를 -260 으로 잘랐다. 그게
             * **물 밖으로 나오는 힘 자체를 없앴다** — 반동으로 튀어오르는
             * 일이 일어날 수가 없었다. 자르지 않는다. */
            /* 물살에 실리는 정도. 3.0 이면 곧바로 실려 나뭇잎처럼 보인다. */
            vC += ((svc / n) - vC) * (1.3f * bdt / (1.0f + 1.3f * bdt));
            /* 비탈을 타고 내려간다. g·sinθ 가 그대로 옆 가속도다 — 계수를
             * 따로 두지 않았다. 물리가 정해주는 값이라 만질 이유가 없다. */
            vC += BOAT_G * slope * bdt;
        } else {
            vA += BOAT_G * bdt;          /* 공중 — 수면은 아무 힘도 안 준다 */
            /* 공중에선 물살에 거의 안 실린다. 아예 0 으로 두면 옆속도가
             * 영영 안 죽어서 통 벽을 타고 떠다닌다. */
            if (n) vC += ((svc / n) - vC) * (0.35f * bdt / (1.0f + 0.35f * bdt));
        }
        bA += vA * bdt;
        bC += vC * bdt;
    }

    /* 중력 틀에서 화면 좌표로 되돌린다. G_ALONG/G_CROSS 의 역이다 —
     * (gux,guy) 가 단위벡터라 회전이므로 이 식이 정확히 맞는다. */
    s_boat_x = CX + bA * s_gux + bC * s_guy;
    s_boat_y = CY + bA * s_guy - bC * s_gux;
    s_boat_vx = vC * s_guy + vA * s_gux;
    s_boat_vy = -vC * s_gux + vA * s_guy;
    /* 통 안에 둔다 */
    float bdx = s_boat_x - CX, bdy = s_boat_y - CY;
    float bd = sqrtf(bdx * bdx + bdy * bdy);
    if (bd > R - 40) {
        float k = (R - 40) / bd;
        s_boat_x = CX + bdx * k;
        s_boat_y = CY + bdy * k;
        s_boat_vx *= -0.3f;
        s_boat_vy *= -0.3f;
    }

    /* 🔋 잔잔하면 쉰다. 세 번에 한 번만 풀어도 눈에는 똑같고,
     * 그동안 CPU 는 쉰다. 흔들거나 기울이면 곧바로 돌아온다. */
    /* 🚨 잔잔하면 세 번에 한 번만 그려 배터리를 아끼는데, 프레임이 이미
     * 느리면 그게 0.5초에 한 장이 되어 "멈췄다" 로 보인다(0909 제보).
     * 프레임이 넉넉할 때만 건너뛴다. 느릴 땐 아낄 여유가 없다. */
    if (s_still_flag && dt < 0.070f) {
        static uint8_t idle;
        if (++idle % 3) return;
    }

    /* 🚨 어디서 시간을 쓰는지 갈라 봐야 고칠 데를 안다 — 입자 푸는 쪽인가
     * 화면 만드는 쪽인가. 뭉뚱그린 숫자로는 엉뚱한 데를 깎게 된다. */
    int64_t _tp = esp_timer_get_time();
    build_spans();
    int64_t _ts = esp_timer_get_time();
    paint_img();
    /* 🚨 화면 전체를 무효화하면 LVGL 이 466줄을 전부 배경칠 → 그림복사 →
     * DMA 한다. 물은 대개 아래쪽 절반에만 있으니 그 셋을 통째로 두 배 낭비한
     * 셈이다(0909: 전송 78ms, 그런데 한 장 전송의 하한은 21.7ms 다).
     * 바뀐 자리만 무효화한다 — 지난번 자리와 이번 자리를 합쳐야 물이 빠진
     * 자리도 지워진다. 배는 물 밖으로도 나가므로 넉넉히 물린다. */
    {
        /* 이번에 뭔가 그려질 자리 = 물 띠 + 배가 차지하는 띠.
         * 🚨 배는 돌면 세로로 길어진다. 돛까지 로컬 y 가 -52~+6 이고 90도
         * 돌면 그게 가로 길이(-30~+33)로 바뀌므로 넉넉히 ±60 을 준다. */
        int y0 = s_img_y0, y1 = s_img_y1;
        int by0 = (int)s_boat_y - 60, by1 = (int)s_boat_y + 60;
        if (by0 < y0) y0 = by0;
        if (by1 > y1) y1 = by1;
        if (y0 < 0) y0 = 0;
        if (y1 > 465) y1 = 465;

        /* 🚨 지난 프레임이 차지했던 자리를 통째로 합쳐야 한다. 물 띠만
         * 합쳤더니 배가 지나간 자리와 빠진 물이 한동안 안 지워졌다
         * (0909 제보: "일부 요소가 지워지는 데 시간이 걸린다"). */
        static int py0 = 0, py1 = 465;
        int i0 = y0 < py0 ? y0 : py0;
        int i1 = y1 > py1 ? y1 : py1;
        py0 = y0; py1 = y1;

        lv_area_t inv = { 0, i0, 465, i1 };
        lv_obj_invalidate_area(s_field, &inv);
    }
    int64_t _te = esp_timer_get_time();
    static int64_t phys, span, pnt; static uint32_t cnt; static int64_t since2;
    phys += _tp - _t0; span += _ts - _tp; pnt += _te - _ts; cnt++;
    if (!since2) since2 = _te;
    if (_te - since2 >= 3000000) {
        ESP_LOGI("water", "가름 %u번 — 넘기기 %llu · 토막 %llu · 칠하기 %llu us"
                 " | 코어1 %u us (걸음 %d)",
                 (unsigned)cnt, (unsigned long long)(phys / cnt),
                 (unsigned long long)(span / cnt), (unsigned long long)(pnt / cnt),
                 (unsigned)s_phys_us, s_phys_sub);
        phys = span = pnt = 0; cnt = 0; since2 = _te;
    }
    frame_tick("water", _t0);
}

static void tap_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        /* 시뮬엔 IMU 가 없다. 손가락 쪽으로 기울인 셈 친다. */
        lv_indev_t *in = lv_indev_active();
        lv_point_t p = { CX, CY };
        if (in) lv_indev_get_point(in, &p);
        water_sim_tilt(p.x, p.y);
        return;
    }
    if (code == LV_EVENT_RELEASED) { water_sim_tilt(CX, CY); return; }
}

/* PSRAM 에서 한꺼번에 잡는다. 못 잡으면 물은 건너뛴다. */
static bool alloc_all(void)
{
    #define GET(v, n) do { v = heap_caps_malloc((n), MALLOC_CAP_SPIRAM); \
                           if (!(v)) return false; } while (0)
    /* 🚨 입자 배열을 내부 RAM 으로 옮겨봤다가 되돌렸다(0909). 걸음 하나가
     * 26ms 인 게 PSRAM 을 기다리는 값이라고 봤는데, 옮겨도 27.6ms 로 그대로
     * 였다 — 기다리는 게 아니라 진짜 계산이 비싼 것이다. 대신 내부 힙이
     * 89 → 72KB, 최대 덩어리가 50 → 32KB 로 쪼그라들어 그리기가 큰 버퍼를
     * 못 잡을 위험만 늘었다. 재보고 아니면 되돌린다. */
    GET(s_x, NP * 4);      GET(s_y, NP * 4);
    GET(s_px, NP * 4);     GET(s_py, NP * 4);
    GET(s_vx, NP * 4);     GET(s_vy, NP * 4);
    GET(s_next, NP * 2);
    GET(s_dens, (size_t)NROW * NCOL);
    GET(s_chur, (size_t)NROW * NCOL);
    GET(s_sp0, (size_t)466 * SPANS * 2);
    GET(s_sp1, (size_t)466 * SPANS * 2);
    GET(s_spn, 466);       GET(s_sfrac, 466);
    GET(s_slope, 466);     GET(s_foam, 466);
    GET(s_cur, 466 * (int)sizeof(int));
    GET(s_scol, 466 * 3 * 2);
#ifdef BADGE_SIM
    /* 코어가 하나라 베낄 이유가 없다 — 같은 자리를 가리킨다 */
    s_rx = s_x; s_ry = s_y; s_rvx = s_vx; s_rvy = s_vy;
#else
    GET(s_rx, NP * 4);     GET(s_ry, NP * 4);
    GET(s_rvx, NP * 4);    GET(s_rvy, NP * 4);
    memcpy(s_rx,  s_x,  NP * sizeof(float));
    memcpy(s_ry,  s_y,  NP * sizeof(float));
    memcpy(s_rvx, s_vx, NP * sizeof(float));
    memcpy(s_rvy, s_vy, NP * sizeof(float));
#endif
    GET(s_img, (size_t)466 * 466 * 2);
    #undef GET
    depth_lut_build();
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = 466;
    s_img_dsc.header.h      = 466;
    s_img_dsc.header.stride = 466 * 2;
    s_img_dsc.data          = (const uint8_t *)s_img;
    s_img_dsc.data_size     = (size_t)466 * 466 * 2;
    return true;
}

static void free_all(void)
{
    #define PUT(v) do { if (v) { heap_caps_free(v); v = NULL; } } while (0)
    PUT(s_x); PUT(s_y); PUT(s_px); PUT(s_py); PUT(s_vx); PUT(s_vy);
    PUT(s_next); PUT(s_dens); PUT(s_chur);
    PUT(s_sp0); PUT(s_sp1); PUT(s_spn); PUT(s_sfrac); PUT(s_slope); PUT(s_foam);
    PUT(s_cur); PUT(s_scol);
#ifdef BADGE_SIM
    s_rx = s_ry = s_rvx = s_rvy = NULL;      /* 남의 자리를 가리키고 있었다 */
#else
    PUT(s_rx); PUT(s_ry); PUT(s_rvx); PUT(s_rvy);
#endif
    PUT(s_img);
    #undef PUT
}

lv_timer_t *water_start(lv_obj_t *root)
{
    s_root = root;
    if (!alloc_all()) {
        free_all();
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, "no memory");
        lv_obj_center(l);
        return NULL;
    }

    /* 아래 절반에 격자로 깐다. 줄마다 반 칸씩 밀어야 줄무늬가 안 보인다. */
    int n = 0;
    for (int row = 0; row < 44 && n < NP; row++) {
        float y = CY - 14 + row * 10.2f;
        for (int col = -23; col <= 23 && n < NP; col++) {
            float x = CX + col * 10.2f + (row & 1 ? 5.1f : 0);
            float dx = x - CX, dy = y - CY;
            if (dx * dx + dy * dy > (float)(R - 8) * (R - 8)) continue;
            s_x[n] = x; s_y[n] = y;
            s_vx[n] = s_vy[n] = 0;
            n++;
        }
    }
    for (; n < NP; n++) { s_x[n] = CX; s_y[n] = CY + 120; s_vx[n] = s_vy[n] = 0; }

    /* 🚨 기준 밀도는 이 배치에서 직접 잰다. 상수로 박아두면 입자 수나
     * 간격을 바꾸는 순간 물이 부풀거나 꺼진다. 가장자리는 이웃이 모자라
     * 낮게 나오니 안쪽 것들만 센다. */
    grid_build_from_current();
    float sum = 0; int cnt = 0;
    for (int i = 0; i < NP; i++) {
        float dx = s_x[i] - CX, dy = s_y[i] - CY;
        if (dx * dx + dy * dy > (float)(R - 40) * (R - 40)) continue;
        float rho = 0;
        for (int j = 0; j < NP; j++) {
            if (j == i) continue;
            float ex = s_x[j] - s_x[i], ey = s_y[j] - s_y[i];
            float r2 = ex * ex + ey * ey;
            if (r2 >= HR2 || r2 < 1e-4f) continue;
            float q = 1.0f - sqrtf(r2) / HR;
            rho += q * q;
        }
        sum += rho; cnt++;
    }
    s_rho0 = cnt ? sum / cnt : 3.0f;

    s_field = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_field);
    lv_obj_set_size(s_field, 466, 466);
    lv_obj_center(s_field);
    lv_obj_clear_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_field, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_field, cover_cb, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_field, tap_cb, LV_EVENT_RELEASED, NULL);

    s_hint = lv_label_create(s_root);
    lv_label_set_text(s_hint, "tilt");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A6478), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 200);

    s_gx = 0; s_gy = 1400.0f;
    s_boat_x = CX; s_boat_y = CY - 20;
    s_boat_vx = s_boat_vy = 0;
    s_last_ms = 0;
#ifndef BADGE_SIM
    /* 🚨 코어1 에 못 박는다. 메인도 BLE 도 코어0 이라 거기가 비어 있다.
     * 우선순위는 LVGL 보다 낮게 — 물리가 늦어도 화면은 돌아야 한다. */
    if (!s_phys) {
        s_go   = xSemaphoreCreateBinary();
        s_done = xSemaphoreCreateBinary();
        s_inflight = false;
        if (!s_go || !s_done ||
            xTaskCreatePinnedToCore(phys_task, "water", 4096, NULL, 3, &s_phys, 1) != pdPASS) {
            ESP_LOGI("water", "코어1 태스크를 못 띄웠다 — 코어0 에서 같이 돈다");
            s_phys = NULL;
        }
    }
#endif
    s_loop = lv_timer_create(step, 33, NULL);
    return s_loop;
}

void water_stop(void)
{
    s_loop = NULL;               /* 타이머는 부른 쪽이 지운다 */
    /* 🚨 그림을 먼저 지우고 메모리를 놓는다. 거꾸로 하면 아직 살아 있는
     * s_field 가 이미 놓아버린 입자 배열을 그리려 든다 — 홈 버튼을 누르면
     * 그 사이에 한 번 더 그려서 죽었다(0909). 달·지구에서 똑같이 겪고
     * 고쳤는데 물에서 반복했다. */
    if (s_field) { lv_obj_delete(s_field); s_field = NULL; }
    s_root = s_hint = NULL;
    /* 🚨 코어1 이 배열을 만지는 중일 수 있다. 판이 끝난 것을 보고 태스크를
     * 없앤 뒤에 놓는다. 거꾸로 하면 이미 놓은 자리를 만진다 — 그리기에서
     * 겪은 것과 같은 사고다. */
    phys_wait();
#ifndef BADGE_SIM
    if (s_phys) { vTaskDelete(s_phys); s_phys = NULL; }
    if (s_go)   { vSemaphoreDelete(s_go);   s_go = NULL; }
    if (s_done) { vSemaphoreDelete(s_done); s_done = NULL; }
#endif
    free_all();                  /* PSRAM 을 꼭 돌려준다 */
}

/* 시뮬에서 손가락 쪽으로 기울어지게 — 실기 IMU 대신 */
void water_sim_tilt(int x, int y)
{
    s_fake_gx = -((float)y - CY) * 4.0f;
    s_fake_gy = -((float)x - CX) * 4.0f;
}

/* 검증용 — 중력 각도, 배 위치, 통 밖으로 샌 입자 수 */
void water_debug(float *deg, float *boat, float *escaped)
{
    if (deg)  *deg  = atan2f(s_gx, s_gy) / DEG2RAD;
    if (boat) *boat = s_boat_x - CX;
    if (escaped) {
        int n = 0;
        for (int i = 0; i < NP; i++) {
            float dx = s_x[i] - CX, dy = s_y[i] - CY;
            if (dx * dx + dy * dy > (float)R * R + 1) n++;
        }
        *escaped = (float)n;
    }
}
