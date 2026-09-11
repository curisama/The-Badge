/* 달·지구 — 손가락으로 돌리는 구.
 *
 * 3D 로 구를 그리지 않는다. 적도 전개도(가로 = 경도, 세로 = 위도) 한 장을
 * 두고, 화면의 각 픽셀에 대해 "이 점은 전개도의 어디인가"를 거꾸로 찾아
 * 가져온다. 그 대응은 자세와 무관하므로 부팅 때 한 번 계산해 PSRAM 에
 * 올려두고, 회전은 가로 방향 오프셋을 더하는 것뿐이다 — 매 프레임에는
 * 곱셈도 삼각함수도 없다.
 *
 * 🚨 캔버스는 PSRAM 에 둔다(466x466x2 = 434KB). 이건 괜찮다 — 예전에
 * 그리기가 13,943번 실패한 건 LVGL 이 *화면으로 밀어내는* 버퍼가 PSRAM 에
 * 있어서 DMA 를 못 했기 때문이다. 캔버스는 밀어내는 버퍼가 아니라 CPU 가
 * 읽어가는 그림 원본이라 PSRAM 이어도 된다. 밀어내는 버퍼는 여전히
 * 내부 RAM 16줄짜리 그대로다.
 *
 * 🚨 전력. 화면 전체를 계속 다시 그리는 건 이 기기에서 제일 비싼 짓이다
 * (정적 화면만으로도 꺼둘 때의 7.3배를 먹는다). 그래서 돌 때만 그린다 —
 * 손가락을 떼면 마찰로 멈추고, 멈추면 한 픽셀도 안 그린다.
 *
 * 🚨 지금 전개도는 코드로 만든 것이다. 진짜 NASA 사진을 쓰려면
 * tools/make-orb-texture.py 로 이미지를 구워 넣으면 된다.
 */
#include "app.h"
#include "port.h"
#include "orb.h"
#include <math.h>
#include <string.h>
#ifdef BADGE_SIM
/* 시뮬엔 PSRAM 도 esp_timer 도 없다. 같은 이름으로 대체를 둔다. */
#  include <stdlib.h>
#  include <stdio.h>
#  include <sys/time.h>
#  define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
static int64_t esp_timer_get_time(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
#  define ESP_LOGE(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#  define ESP_LOGI(tag, ...) do { fprintf(stderr, "[%s] ", tag); \
                                  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#  include "esp_heap_caps.h"
#  include "esp_timer.h"
#  include "esp_log.h"
#endif

#define CX      233
#define CY      233
#define ORB_R   206              /* 구의 반지름 */
#define TEX_W   512              /* 2의 거듭제곱 — 경도 감싸기를 &로 한다 */
#define TEX_H   256

/* 🚨 진짜 사진을 쓰려면 tools/make-orb-texture.py 로 구워 넣으면 된다.
 * 구워둔 게 있으면 플래시에서 그대로 읽는다 — PSRAM 260KB 를 안 쓴다.
 * 없으면 아래에서 코드로 만든다(달은 그럴듯하고, 지구는 지구가 아니다). */
#if __has_include("assets/orb_tex_moon.h")
#  include "assets/orb_tex_moon.h"
#  define HAVE_MOON_TEX 1
#endif
#if __has_include("assets/orb_tex_earth.h")
#  include "assets/orb_tex_earth.h"
#  define HAVE_EARTH_TEX 1
#endif
#if __has_include("assets/orb_tex_sun.h")
#  include "assets/orb_tex_sun.h"
#  define HAVE_SUN_TEX 1
#endif
#if __has_include("assets/orb_tex_jupiter.h")
#  include "assets/orb_tex_jupiter.h"
#  define HAVE_JUPITER_TEX 1
#endif

const char *const ORB_NAME[ORB_N] = { "Moon", "Earth", "Sun", "Jupiter" };

static lv_obj_t   *s_canvas;
static lv_timer_t *s_loop;
static uint16_t   *s_fb;         /* 캔버스 그림 (PSRAM) */
static const uint16_t *s_tex;    /* 전개도 RGB565 — 플래시이거나 아래 것 */
/* 🚨 이 둘을 한 변수로 겸하면 안 된다. "내가 잡았으니 놓아야 한다" 와
 * "코드로 만들어야 한다" 는 다른 말인데, 전개도를 PSRAM 으로 옮기면서
 * 겸하게 뒀더니 옮겨놓은 진짜 사진 위에 만든 그림을 덮어썼다 — 태양도
 * 목성도 지구로 나왔다(0909 지적). */
static uint16_t   *s_tex_owned;  /* 내가 잡았다 — 나갈 때 놓아야 한다 */
static bool        s_tex_bake;   /* 구워둔 사진이 없다 — 코드로 만들어야 한다 */
/* 🚨 가로 회전은 표를 그대로 두고 경도에 값만 더하면 된다. 세로는 다르다 —
 * 축이 기울면 픽셀마다 위·경도가 통째로 바뀌어서, 매 프레임 역삼각함수를
 * 145,000번 풀어야 한다(한 번에 수백 사이클이라 프레임당 0.4초다. 불가능).
 *
 * 대신 8픽셀 간격 격자에서만 정확히 풀고 사이는 이어서 채운다. 구면이라
 * 그 사이는 매끄러워서 티가 안 난다. 격자는 2,916점 — 145,000번이 2,916번이
 * 된다. 그리고 격자는 '세로 기울기'에만 달렸으므로, 가로로만 돌 때는
 * 다시 계산할 일이 아예 없다. */
/* 픽셀마다 정확히 푼다. 성긴 격자에서 풀고 사이를 이어 채워봤더니, 원의
 * 위아래에서 위도가 픽셀 하나에 확 변하는 바람에 세로로 죽죽 뭉갰다
 * (0909 실측). 격자를 촘촘히 해도 가장자리는 기울기가 무한대라 못 이긴다.
 *
 * 대신 역삼각함수를 싼 근사로 바꾼다. 표준 라이브러리 것은 한 번에 수백
 * 사이클이라 145,000번은 무리지만, atan2 는 다항식 근사로 15번 남짓,
 * asin 은 표를 찾아 이으면 몇 번이면 끝난다.
 *
 * 픽셀마다 다시 잡는 건 z(구의 깊이)뿐이라 그것만 미리 재둔다. */
static int16_t    *s_lut_z;      /* 픽셀마다 구의 z (32767 = 1.0) */
static float      *s_col_x;      /* 열마다 x (-1~1) */
static float      *s_asin_tab;   /* asin 표 (1025칸) */
static uint8_t    *s_lut_shade;  /* 픽셀마다 밝기 0~255 — 기울기와 무관하다 */

/* atan2 근사 — 오차 1e-5 아래. 나눗셈 하나에 곱셈 몇 번뿐이다. */
static inline float atan2_fast(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float d = (ax > ay ? ax : ay) + 1e-12f;
    float a = (ax > ay ? ay : ax) / d;
    float ss = a * a;
    float r = ((-0.0464964749f * ss + 0.15931422f) * ss - 0.327622764f) * ss * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0)   r = 3.14159274f - r;
    return y < 0 ? -r : r;
}
static int32_t    *s_span0, *s_span1;   /* 줄마다 구가 차지하는 x 범위 */
static float       s_lon;        /* 지금 경도 (0~1) */
static float       s_spin;       /* 초당 회전 (바퀴/초) */
static float       s_tilt;       /* 세로로 기울인 각(라디안) */
static float       s_tilt_v;     /* 세로 여세 */
/* 🚨 축이 옆으로 기우는 각. 이게 없으면 회전판이라 축이 화면 세로에
 * 못 박힌다. 구는 무늬가 대각선으로 흘러 그럴싸해 보이지만, 토성 고리는
 * 축을 중심으로 완벽한 대칭이라 제자리 돌리기에 아무 반응이 없다 —
 * 고리가 열리고 닫히기만 하고 긴 축이 영영 가로였다(0908 지적). */
static float       s_lean;        /* 축이 옆으로 기운 각(라디안) */
static float       s_lean_v;      /* 그 여세 */
static bool        s_coarse;      /* 움직이는 중 — 굵게 그린다 */
/* 🚨 이 표가 실제로 이득인지 **기기에서 안 재봤다**(0909, 배지를 들고 나감).
 * PC 시뮬에선 오히려 느렸다 — x86 은 삼각함수가 싸고 캐시가 커서 표를 읽는
 * 값이 더 비싸다. 기기는 반대일 수 있다(삼각함수가 비싸고 캐시가 32KB).
 * 그런데 진짜 벽이 계산이 아니라 **사진 읽기** 일 가능성도 있다 — 256KB
 * 전개도를 픽셀마다 건너뛰며 읽으니 캐시가 계속 빗나간다. 그렇다면 이 표는
 * 아무것도 못 줄이고 PSRAM 532KB 만 쓴다.
 * → 재보고 이득이 없으면 아래를 0 으로. 그 한 줄이면 통째로 빠진다.
 *   같이 재볼 것: 전개도를 512x256 에서 256x128 로 줄이면 어떤가(캐시에
 *   훨씬 잘 맞는다). 그쪽이 진짜 답일 수 있다.
 *
 * 픽셀마다 역삼각함수를 푸는 게 이 그림에서 제일 비싸다. 그런데 가로로
 * 돌리기만 할 땐 그 답이 안 바뀐다 — 경도는 정수로 더하는 것뿐이라서다.
 * 자세(눕힌 각·기운 각)가 그대로면 픽셀마다의 전개도 좌표를 표에 담아두고
 * 다음 프레임부턴 표만 본다. 자세가 바뀌면 그 프레임에 다시 채운다. */
#define ORB_UV_TABLE 1           /* 0 으로 두면 표를 아예 안 쓴다 */
static uint16_t   *s_lut_tu;     /* 픽셀마다 전개도 가로칸 (경도 더하기 전) */
static uint16_t   *s_lut_tv;     /* 픽셀마다 전개도 세로칸 */
static bool        s_uv_ok;      /* 표가 지금 자세와 맞나 */
static float       s_uv_tilt, s_uv_lean;
static uint32_t    s_last_ms;
static uint32_t    s_render_us;
static bool        s_dirty;
static int32_t     s_drag_x, s_drag_y;
static bool        s_dragging;
static orb_kind_t  s_kind;

/* ── 잡음 ─────────────────────────────────────────────────────
 * 값 잡음을 여러 배율로 겹친다. 경도 방향은 감싸져야 이음매가 안 보인다. */
static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)(x * 374761393) + (uint32_t)(y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float vnoise(float x, float y, int period)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    #define N(a,b) ((float)(hash2(((a) % period + period) % period, b) & 0xFFFF) / 65535.0f)
    float n00 = N(xi, yi),     n10 = N(xi + 1, yi);
    float n01 = N(xi, yi + 1), n11 = N(xi + 1, yi + 1);
    #undef N
    return (n00 * (1 - u) + n10 * u) * (1 - v) + (n01 * (1 - u) + n11 * u) * v;
}
static float fbm(float x, float y, int oct, int period)
{
    float sum = 0, amp = 0.5f, tot = 0;
    int p = period;
    for (int i = 0; i < oct; i++) {
        sum += vnoise(x, y, p) * amp;
        tot += amp;
        x *= 2; y *= 2; p *= 2; amp *= 0.5f;
    }
    return sum / tot;
}

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ── 전개도 만들기 ────────────────────────────────────────────
 * 🚨 코드로 만든 것이다. 달은 회색조에 크레이터라 이 방식으로도 꽤 그럴듯
 * 하지만, 지구는 대륙 모양이 실제와 다르다 — "지구 비슷한 행성"이지
 * 지구가 아니다. 진짜를 쓰려면 tools/make-orb-texture.py 를 써라. */
static void bake_moon(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            /* 바탕 — 잔잔한 회색 */
            float base = 0.62f + 0.16f * fbm(fx * 8, fy * 8, 5, 8);
            /* 바다(어두운 현무암 평원) — 큰 덩어리로 몇 군데 */
            float mare = fbm(fx * 3 + 11.3f, fy * 3 + 4.7f, 3, 3);
            if (mare < 0.42f) base *= 0.62f + 0.5f * (mare / 0.42f);
            /* 크레이터 — 격자마다 하나씩 두고 가장자리를 밝게 */
            float c = 0;
            for (int lvl = 0; lvl < 3; lvl++) {
                int per = 12 << lvl;
                float gx = fx * per, gy = fy * per;
                int cx = (int)floorf(gx), cy = (int)floorf(gy);
                for (int oy = -1; oy <= 1; oy++)
                    for (int ox = -1; ox <= 1; ox++) {
                        uint32_t h = hash2(((cx + ox) % per + per) % per, cy + oy);
                        float px = cx + ox + (float)(h & 0xFF) / 255.0f;
                        float py = cy + oy + (float)((h >> 8) & 0xFF) / 255.0f;
                        float rad = 0.18f + 0.26f * (float)((h >> 16) & 0xFF) / 255.0f;
                        if (((h >> 24) & 0xFF) < 150) continue;      /* 다 파진 않는다 */
                        float d = sqrtf((gx - px) * (gx - px) + (gy - py) * (gy - py));
                        if (d > rad) continue;
                        float t = d / rad;
                        /* 안쪽은 어둡게, 테두리는 밝게 */
                        c += (t > 0.78f) ? (0.20f * (1 - fabsf(t - 0.89f) / 0.11f))
                                         : (-0.16f * (1 - t));
                    }
            }
            int v = (int)((base + c) * 255.0f);
            s_tex_owned[y * TEX_W + x] = rgb565(v, v, (int)(v * 0.97f));
        }
    }
}

static void bake_earth(void)
{
    for (int y = 0; y < TEX_H; y++) {
        float fy = (float)y / TEX_H;
        float lat = (0.5f - fy) * 2.0f;              /* +1 북극 ~ -1 남극 */
        for (int x = 0; x < TEX_W; x++) {
            float fx = (float)x / TEX_W;
            float h = fbm(fx * 6, fy * 6, 6, 6);
            /* 위도가 높을수록 육지가 덜 나오게 살짝 눌러 대륙처럼 뭉친다 */
            h -= 0.10f * fabsf(lat);
            int r, g, b;
            if (h < 0.46f) {                          /* 바다 — 깊이에 따라 */
                float d = h / 0.46f;
                r = (int)(6 + 18 * d); g = (int)(28 + 60 * d); b = (int)(78 + 90 * d);
            } else {                                  /* 육지 */
                float t = (h - 0.46f) / 0.54f;
                r = (int)(48 + 70 * t); g = (int)(96 + 60 * t); b = (int)(44 + 40 * t);
                /* 사막 — 경계를 칼같이 자르면 블록처럼 보인다. 섞는다. */
                float dry = fbm(fx * 11 + 5.5f, fy * 11 + 2.5f, 4, 11);
                float band = 1.0f - fabsf(lat) / 0.40f;          /* 적도 근처일수록 */
                float k = (dry - 0.50f) / 0.22f * (band > 0 ? band : 0);
                if (k > 0) {
                    if (k > 1) k = 1;
                    r = (int)(r * (1 - k) + 178 * k);
                    g = (int)(g * (1 - k) + 150 * k);
                    b = (int)(b * (1 - k) +  98 * k);
                }
                if (t > 0.62f) {                                  /* 산 */
                    float m = (t - 0.62f) / 0.38f;
                    int sv = (int)(150 + 90 * m);
                    r = (int)(r * (1 - m) + sv * m);
                    g = (int)(g * (1 - m) + sv * m);
                    b = (int)(b * (1 - m) + sv * m);
                }
            }
            /* 만년설 */
            float ice = (fabsf(lat) - 0.78f) / 0.22f;
            if (ice > 0) {
                float k = ice > 1 ? 1 : ice;
                r = (int)(r * (1 - k) + 244 * k);
                g = (int)(g * (1 - k) + 248 * k);
                b = (int)(b * (1 - k) + 255 * k);
            }
            /* 구름 한 겹 */
            float cl = fbm(fx * 9 + 21.1f, fy * 9 + 13.7f, 5, 9);
            if (cl > 0.58f) {
                float k = (cl - 0.58f) / 0.42f; if (k > 0.8f) k = 0.8f;
                r = (int)(r * (1 - k) + 250 * k);
                g = (int)(g * (1 - k) + 250 * k);
                b = (int)(b * (1 - k) + 252 * k);
            }
            s_tex_owned[y * TEX_W + x] = rgb565(r, g, b);
        }
    }
}

/* ── 조회 테이블 ──────────────────────────────────────────────
 * 화면의 한 점 (dx,dy) 가 구의 어느 위·경도인가. 자세가 안 변하므로
 * 한 번만 계산한다. 회전은 경도에 더하는 것뿐이다. */
static bool build_lut(void)
{
    size_t px = (size_t)(2 * ORB_R + 1) * (2 * ORB_R + 1);
    s_lut_shade = heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
    s_lut_z     = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    s_span0     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_span1     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_col_x     = heap_caps_malloc((2 * ORB_R + 1) * 4, MALLOC_CAP_SPIRAM);
    s_asin_tab  = heap_caps_malloc(1025 * 4, MALLOC_CAP_SPIRAM);
#if ORB_UV_TABLE
    s_lut_tu    = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    s_lut_tv    = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
#else
    s_lut_tu = s_lut_tv = NULL;
#endif
    s_uv_ok = false;
    if (!s_lut_shade || !s_lut_z || !s_span0 || !s_span1 || !s_col_x || !s_asin_tab)
        return false;
    /* 전개도 좌표 표는 없어도 돌아간다 — 느릴 뿐이다. 없으면 없는 대로 간다. */
    if (!s_lut_tu || !s_lut_tv) {
        if (s_lut_tu) { heap_caps_free(s_lut_tu); s_lut_tu = NULL; }
        if (s_lut_tv) { heap_caps_free(s_lut_tv); s_lut_tv = NULL; }
    }

    for (int i = 0; i <= 2 * ORB_R; i++) s_col_x[i] = (float)(i - ORB_R) / ORB_R;
    for (int i = 0; i <= 1024; i++) {
        float t = (float)i / 512.0f - 1.0f;
        if (t >  1) t =  1;
        if (t < -1) t = -1;
        s_asin_tab[i] = asinf(t);
    }

    /* 빛은 왼쪽 위 앞에서. 정면광이면 평평해 보인다.
     * 🚨 밝기는 화면에서 본 법선으로 정해진다 — 지구를 어느 쪽으로 돌리든
     * 그 픽셀이 공의 어디쯤인지는 안 바뀐다. 그래서 한 번만 구하면 된다. */
    /* 🚨 yy 를 '화면 위가 +' 로 잡는다. 예전엔 픽셀 좌표 그대로(아래가 +)
     * 써서 위도가 통째로 뒤집혔다 — 화면 위쪽이 남극이었다(0909 발견).
     * 양쪽 극이 다 하얘서 눈으로는 안 보이고, 대륙도 뒤집히면 못 알아본다.
     * 빛도 같은 좌표로 맞춘다(왼쪽 위 앞에서). */
    const float lx = -0.45f, ly = 0.42f, lz = 0.79f;

    for (int j2 = 0; j2 <= 2 * ORB_R; j2++) {
        int dy = j2 - ORB_R;
        float yy = -(float)dy / ORB_R;      /* 화면 위 = 북쪽 */
        float rowr2 = 1.0f - yy * yy;
        if (rowr2 <= 0) { s_span0[j2] = 1; s_span1[j2] = 0; continue; }
        int half = (int)(ORB_R * sqrtf(rowr2));
        s_span0[j2] = ORB_R - half;
        s_span1[j2] = ORB_R + half;

        for (int i2 = s_span0[j2]; i2 <= s_span1[j2]; i2++) {
            int dx = i2 - ORB_R;
            float xx = (float)dx / ORB_R;
            float zz2 = 1.0f - xx * xx - yy * yy;
            if (zz2 < 0) zz2 = 0;
            float zz = sqrtf(zz2);

            /* 🚨 태양은 스스로 빛난다 — 그림자가 지면 안 된다.
             * 가장자리만 살짝 눌러 공처럼 보이게 하고 밝기는 그대로 둔다. */
            if (s_kind == ORB_SUN) {
                s_lut_shade[(size_t)j2 * (2 * ORB_R + 1) + i2] =
                    (uint8_t)((0.88f + 0.12f * zz) * 255.0f);
                s_lut_z[(size_t)j2 * (2 * ORB_R + 1) + i2] = (int16_t)(zz * 32767.0f);
                continue;
            }
            float nl = xx * lx + yy * ly + zz * lz;   /* 램버트 */
            if (nl < 0) nl = 0;
            /* 위성 사진에는 이미 햇빛이 들어 있다. 세게 곱하면 두 번 어두워진다. */
            /* 🚨 지구만 더 밝게 든다. 텍스처 밝기를 재보니 지구가 유독 어둡다
             * (0910 실측, 중앙값 255 중에):
             *     지구 41.3 · 달 140.4 · 목성 159.0 · 태양 148.7
             * 바다가 화면 대부분인데 그게 아주 어둡고 구름만 243 이라 평균만
             * 올려놓은 탓이다. 빛 계산을 다 같이 올리면 달·목성이 떠버리므로
             * 지구만 바탕을 든다. 텍스처 자체도 감마 0.38 로 밝혔다 —
             * 어두운 쪽을 많이, 밝은 쪽은 거의 그대로 올리는 곡선이라
             * 구름이 안 날아간다(포화 9.3% → 10.4%). */
            /* 🚨 감마 0.38 + 바탕 0.55 로 갔다가 "너무 밝다" 는 말을 들었다
             * (0910). 한 단계씩 내렸다 — 텍스처는 실효 감마 0.50, 바탕은 0.50. */
            float amb = (s_kind == ORB_EARTH) ? 0.50f : 0.42f;
            float rim = (s_kind == ORB_EARTH) ? 0.78f : 0.74f;
            float sh = amb + (1.0f - amb) * nl;
            sh *= rim + (1.0f - rim) * zz;            /* 가장자리만 살짝 */
            if (sh > 1.0f) sh = 1.0f;
            size_t idx = (size_t)j2 * (2 * ORB_R + 1) + i2;
            s_lut_shade[idx] = (uint8_t)(sh * 255.0f);
            s_lut_z[idx] = (int16_t)(zz * 32767.0f);
        }
    }
    return true;
}

/* ── 한 프레임 ────────────────────────────────────────────────
 * 하는 일은 테이블 보고 전개도에서 픽셀 긁어와 밝기 곱하는 것뿐이다. */
static void render(void)
{
    int64_t t0 = esp_timer_get_time();
    float ct = cosf(s_tilt), st = sinf(s_tilt);
    /* 🚨 축이 기운 것은 화면을 통째로 돌린 것과 같다. 그래서 마지막에
     * 화면 좌표만 되돌려 주면 나머지 계산은 그대로다 — 표도 그대로 쓴다.
     * 밝기와 깊이는 되돌리지 않는다. 빛은 세상에 붙어 있지 천체를 따라
     * 돌지 않고, 깊이는 중심에서의 거리만 타서 돌려도 그대로다. */
    float cl = cosf(s_lean), sl = sinf(s_lean);
    bool  leaning = (s_lean != 0.0f);
    int   rot = (int)(s_lon * TEX_W);
    int   stride = 2 * ORB_R + 1;

    const float U2TEX = TEX_W / 6.2831853f;
    const float V2TEX = TEX_H / 3.1415927f;

    /* 🚨 픽셀마다 역삼각함수를 푸는 그림이라 한 장에 135ms 가 걸렸다(초당 7장).
     * 손으로 돌리는 동안엔 눈이 세밀함을 못 쫓으니 2×2 덩어리로 그려 계산을
     * 4분의 1로 줄이고, 손을 놓고 멈추면 그때 한 장을 온전히 다시 그린다.
     * 멈춘 그림이 결국 오래 보이는 그림이라, 거기서만 또렷하면 된다. */
    /* 자세가 그대로면 표만 보고 그린다 — 픽셀마다 풀던 역삼각함수가 통째로
     * 빠진다. 이땐 굵게 그릴 이유도 없으니 온전한 해상도로 간다. */
    bool use_uv = (s_lut_tu && s_uv_ok && s_uv_tilt == s_tilt && s_uv_lean == s_lean);
    bool fill_uv = (s_lut_tu && !use_uv && !s_coarse);
    int step = (s_coarse && !use_uv) ? 2 : 1;

    for (int j = 0; j <= 2 * ORB_R; j += step) {
        int y = CY - ORB_R + j;
        if (y < 0 || y >= 466) continue;
        int i0 = s_span0[j], i1 = s_span1[j];
        if (i0 > i1) continue;
        uint16_t *out = s_fb + (size_t)y * 466;
        const uint8_t *sd = s_lut_shade + (size_t)j * stride;
        const int16_t *zl = s_lut_z     + (size_t)j * stride;

        /* y 는 줄마다 한 번만 구하면 된다. 화면 위가 북쪽이다. */
        float yy = -(float)(j - ORB_R) / ORB_R;
        float yc = yy * ct, ys = yy * st;

        /* 기울어 있으면 줄을 따라가며 되돌린 좌표를 더해 나간다 */
        float oy = (float)(j - ORB_R), ox0 = (float)(i0 - ORB_R);
        float inv_R = 1.0f / ORB_R;
        float xr = (ox0 * cl + oy * sl) * inv_R, xr_d =  cl * inv_R;
        float yr = -(-ox0 * sl + oy * cl) * inv_R, yr_d = sl * inv_R;

        uint16_t *ru = s_lut_tu ? s_lut_tu + (size_t)j * stride : NULL;
        uint16_t *rv = s_lut_tv ? s_lut_tv + (size_t)j * stride : NULL;

        /* ── 빠른 길: 표만 보고 그린다 ─────────────────────── */
        if (use_uv) {
            for (int i = i0; i <= i1; i++) {
                int x = CX - ORB_R + i;
                if (x < 0 || x >= 466) continue;
                int tu = (ru[i] + rot) & (TEX_W - 1);
                uint16_t c = s_tex[(size_t)rv[i] * TEX_W + tu];
                uint32_t sh = sd[i];
                uint32_t r = ((c >> 11) & 0x1F) * sh >> 8;
                uint32_t g = ((c >> 5)  & 0x3F) * sh >> 8;
                uint32_t b = ( c        & 0x1F) * sh >> 8;
                out[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
            continue;
        }

        uint16_t first_px = 0, last_px = 0;   /* 굵게 그릴 때 가장자리 메우기용 */
        for (int i = i0; i <= i1; i += step, xr += xr_d * step, yr += yr_d * step) {
            int x = CX - ORB_R + i;
            if (x < 0 || x >= 466) continue;

            float cx = s_col_x[i];
            if (leaning) { cx = xr; yc = yr * ct; ys = yr * st; }

            float zz = zl[i] * (1.0f / 32767.0f);
            /* 세로로 눕힌다(X축 회전). 가로 회전은 아래에서 정수로 더한다. */
            float y2 = yc - zz * st;
            float z2 = ys + zz * ct;

            /* 경도 — 다항식 근사. 경도를 더하기 전 값을 표에 담는다. */
            int tu0 = (int)(atan2_fast(cx, z2) * U2TEX) & (TEX_W - 1);
            int tu = (tu0 + rot) & (TEX_W - 1);

            /* 위도 — 표를 찾아 이어 쓴다 */
            float fa = (y2 + 1.0f) * 512.0f;
            int   ia = (int)fa;
            if (ia < 0) ia = 0;
            if (ia > 1023) ia = 1023;
            float lat = s_asin_tab[ia] + (s_asin_tab[ia + 1] - s_asin_tab[ia]) * (fa - ia);
            int tv = (int)(TEX_H * 0.5f - lat * V2TEX);
            if (tv < 0) tv = 0;
            if (tv >= TEX_H) tv = TEX_H - 1;

            if (fill_uv) { ru[i] = (uint16_t)tu0; rv[i] = (uint16_t)tv; }

            uint16_t c = s_tex[(size_t)tv * TEX_W + tu];
            uint32_t sh = sd[i];
            uint32_t r = ((c >> 11) & 0x1F) * sh >> 8;
            uint32_t g = ((c >> 5)  & 0x3F) * sh >> 8;
            uint32_t b = ( c        & 0x1F) * sh >> 8;
            uint16_t px = (uint16_t)((r << 11) | (g << 5) | b);
            out[x] = px;
            if (i == i0) first_px = px;
            last_px = px;
            if (step == 2) {                 /* 옆·아래로 같은 색을 편다 */
                if (x + 1 < 466 && i + 1 <= i1) out[x + 1] = px;
                if (y + 1 < 466) {
                    uint16_t *o2 = s_fb + (size_t)(y + 1) * 466;
                    int j2 = j + 1;
                    if (j2 <= 2 * ORB_R && i >= s_span0[j2] && i <= s_span1[j2]) {
                        o2[x] = px;
                        if (x + 1 < 466 && i + 1 <= s_span1[j2]) o2[x + 1] = px;
                    }
                }
            }
        }
        /* 🚨 아래 줄이 위 줄보다 넓은 데(극 근처)는 베껴 쓸 색이 없어서
         * 지난 프레임 색이 남는다 — 돌릴 때 테두리가 너덜거린다.
         * 양 끝 색으로 그만큼만 메운다. */
        if (step == 2 && j + 1 <= 2 * ORB_R) {
            int j2 = j + 1, y2 = y + 1;
            if (y2 < 466 && s_span0[j2] <= s_span1[j2]) {
                uint16_t *o2 = s_fb + (size_t)y2 * 466;
                for (int i = s_span0[j2]; i < i0; i++) {
                    int x2 = CX - ORB_R + i;
                    if (x2 >= 0 && x2 < 466) o2[x2] = first_px;
                }
                for (int i = i1 + 1; i <= s_span1[j2]; i++) {
                    int x2 = CX - ORB_R + i;
                    if (x2 >= 0 && x2 < 466) o2[x2] = last_px;
                }
            }
        }
    }
    if (fill_uv) { s_uv_ok = true; s_uv_tilt = s_tilt; s_uv_lean = s_lean; }
    s_render_us = (uint32_t)(esp_timer_get_time() - t0);
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

    uint32_t now = lv_tick_get();
    float dt = s_last_ms ? (now - s_last_ms) / 1000.0f : 0.033f;
    s_last_ms = now;
    if (dt > 0.2f) dt = 0.2f;

    if (!s_dragging) {
        s_spin *= powf(0.35f, dt);            /* 마찰 */
        if (fabsf(s_spin) < 0.004f) s_spin = 0;
        s_lon += s_spin * dt;
        s_tilt_v *= powf(0.30f, dt);
        if (fabsf(s_tilt_v) < 0.01f) s_tilt_v = 0;
        s_tilt += s_tilt_v * dt;
        s_lean_v *= powf(0.30f, dt);
        if (fabsf(s_lean_v) < 0.01f) s_lean_v = 0;
        s_lean += s_lean_v * dt;
    }
    /* 막지 않는다. 계속 넘기면 극을 지나 뒤집혀 반대편을 본다.
     * (격자로 이어 붙이던 시절엔 극 근처가 찢어져 막아뒀는데, 픽셀마다
     *  정확히 푸는 지금은 극을 정면으로 봐도 깨끗하다.) */
    while (s_tilt >  3.1415927f) s_tilt -= 6.2831853f;
    while (s_tilt < -3.1415927f) s_tilt += 6.2831853f;
    while (s_lean >  3.1415927f) s_lean -= 6.2831853f;
    while (s_lean < -3.1415927f) s_lean += 6.2831853f;
    if (s_lon < 0) s_lon += 1.0f;
    if (s_lon >= 1.0f) s_lon -= 1.0f;

    /* 🚨 안 돌면 한 픽셀도 안 그린다. 화면 전체를 계속 다시 그리는 게
     * 이 기기에서 제일 비싼 짓이라, 멈춰 있을 땐 정적 화면과 같아진다.
     * 타이머도 느리게 돌려 CPU 를 덜 깨운다. */
    bool moving = (s_spin != 0 || s_tilt_v != 0 || s_lean_v != 0 || s_dragging);
    /* 🚨 방금 멈췄으면 한 장은 반드시 또렷하게 다시 그린다. 이 판단이 아래
     * '안 돌면 건너뛴다' 보다 먼저 와야 한다 — 뒤에 두면 멈추는 순간 바로
     * 건너뛰어서 굵게 그린 마지막 장이 그대로 남는다. */
    /* 🚨 자세가 바뀌면 표가 못 쓰게 된다. 다음 온전한 프레임에 다시 채운다. */
    if (s_uv_ok && (s_uv_tilt != s_tilt || s_uv_lean != s_lean)) s_uv_ok = false;
    /* 표가 맞으면 온전한 해상도로도 싸게 그린다 — 굵게 갈 이유가 없다 */
    bool can_fast = (s_lut_tu && s_uv_ok);
    if (!moving && s_coarse) { s_dirty = true; }
    s_coarse = moving && !can_fast;

    if (!moving && !s_dirty) {
        /* 안 돌면 그릴 게 없다. 주기는 그대로 두고 6번에 한 번만 본다. */
        static uint8_t idle;
        if (++idle % 6) return;
        return;
    }
    s_dirty = false;
    int64_t _t0 = esp_timer_get_time();
    render();
    lv_obj_invalidate(s_canvas);
    frame_tick("orb", _t0);
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);

    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        s_drag_x = p.x;
        s_drag_y = p.y;
        s_spin = 0;
        s_tilt_v = 0;
        s_lean_v = 0;
    } else if (code == LV_EVENT_PRESSING) {
        int32_t dx = p.x - s_drag_x, dy = p.y - s_drag_y;
        if (dx || dy) {
            s_drag_x = p.x;
            s_drag_y = p.y;
            /* 화면 지름만큼 가로로 끌면 반 바퀴.
             * 🚨 극을 넘겨 뒤집으면 반대편에서 보는 셈이라 경도가 좌우로
             * 뒤집혀 보인다 — 그대로 두면 문댄 방향과 반대로 돈다
             * (0909 지적). 뒤집힌 동안엔 부호를 바꿔 손을 따라가게 한다. */
            /* 🚨 공을 어디를 잡았느냐가 중요하다. 한가운데를 가로로 문대면
             * 제자리에서 돌지만, 꼭대기를 잡고 옆으로 밀면 공이 통째로
             * 기운다 — 진짜 공이 그렇다. 잡은 자리로 둘을 나눈다.
             * 이게 없으면 축이 화면 세로에 못 박혀 있다(0908 지적). */
            float gx = (float)(p.x - CX) / ORB_R;
            float gy = (float)(p.y - CY) / ORB_R;
            if (gx >  1) gx =  1;
            if (gx < -1) gx = -1;
            if (gy >  1) gy =  1;
            if (gy < -1) gy = -1;

            /* 가장자리를 잡고 문대면 공이 그 자리에서 굴러 축이 기운다.
             * 이건 화면에서 보이는 그대로라 되돌리기 전에 센다. */
            float dln = ((float)dx * -gy + (float)dy * gx)
                        / (2.0f * ORB_R) * 3.1415927f;

            /* 🚨 축이 기울어 있으면 손가락 움직임도 그만큼 되돌려야 한다.
             * 돌리기·눕히기는 공의 축을 기준으로 도는데, 화면은 기운 채로
             * 보이기 때문이다. 안 되돌리면 90도쯤에서 가로로 문댄 게
             * 세로가 되고 180도에선 거꾸로 간다 — "달 방향이 또
             * 뒤집혔다"의 정체다(0908 지적). 잡은 자리도 같이 되돌린다. */
            float cl = cosf(s_lean), sl = sinf(s_lean);
            float bx =  (float)dx * cl + (float)dy * sl;
            float by = -(float)dx * sl + (float)dy * cl;
            float bgx =  gx * cl + gy * sl;
            float bgy = -gx * sl + gy * cl;

            float flip = (cosf(s_tilt) >= 0) ? 1.0f : -1.0f;
            float dl = -bx / (2.0f * ORB_R) * 0.5f * flip
                       * (1.0f - fabsf(bgy));
            s_lon += dl;
            s_spin = dl * 12.0f;                       /* 놓을 때의 여세 */

            s_lean += dln;
            s_lean_v = dln * 12.0f;
            /* 세로는 지름만큼 끌면 180도. 대각선이면 둘이 같이 먹는다.
             * 🚨 남북을 바로잡으면서(화면 위 = 북쪽) 세로 방향도 같이
             * 뒤집혔다. 부호를 맞춰 문댄 쪽으로 돌게 한다(0909 지적). */
            float dt2 = -by / (2.0f * ORB_R) * 3.1415927f
                        * (1.0f - fabsf(bgx));
            s_tilt += dt2;
            s_tilt_v = dt2 * 12.0f;
            s_dirty = true;
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_dragging = false;
    }
}

lv_timer_t *orb_start(lv_obj_t *root, orb_kind_t kind)
{
    int64_t _open0 = esp_timer_get_time();
    s_kind = kind;
    s_lon = 0; s_spin = 0.10f;       /* 처음엔 천천히 돌면서 인사한다 */
    s_tilt = 0; s_tilt_v = 0;
    s_lean = 0; s_lean_v = 0;
    s_last_ms = 0; s_dirty = true; s_dragging = false;

    s_fb = heap_caps_malloc((size_t)466 * 466 * 2, MALLOC_CAP_SPIRAM);

    s_tex = NULL;
#ifdef HAVE_MOON_TEX
    if (kind == ORB_MOON)    s_tex = ORB_TEX_MOON;
#endif
#ifdef HAVE_EARTH_TEX
    if (kind == ORB_EARTH)   s_tex = ORB_TEX_EARTH;
#endif
#ifdef HAVE_SUN_TEX
    if (kind == ORB_SUN)     s_tex = ORB_TEX_SUN;
#endif
#ifdef HAVE_JUPITER_TEX
    if (kind == ORB_JUPITER) s_tex = ORB_TEX_JUPITER;
#endif
    s_tex_bake = false;
    if (!s_tex) {   /* 구워둔 사진이 없으면 코드로 만든다 */
        s_tex_owned = heap_caps_malloc((size_t)TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
        s_tex = s_tex_owned;
        s_tex_bake = true;
    } else {
        /* 🚨 구워둔 사진은 플래시에 있다. 픽셀마다 여기저기 건너뛰며 읽는데
         * 플래시는 4선(QSPI)이라 캐시가 빗나갈 때마다 오래 기다린다. PSRAM 은
         * 8선이라 같은 캐시 미스가 절반 값이다. 256KB 를 옮겨 두고 읽는다. */
        uint16_t *fast = heap_caps_malloc((size_t)TEX_W * TEX_H * 2, MALLOC_CAP_SPIRAM);
        if (fast) {
            memcpy(fast, s_tex, (size_t)TEX_W * TEX_H * 2);
            s_tex_owned = fast;
            s_tex = fast;
        }
    }
    if (!s_fb || !s_tex || !build_lut()) {
        ESP_LOGE("orb", "★ 자리가 모자란다 — 달·지구는 건너뛴다");
        orb_stop();
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, "no memory");
        lv_obj_center(l);
        return NULL;
    }
    memset(s_fb, 0, (size_t)466 * 466 * 2);
    if (s_tex_bake) { if (kind == ORB_MOON) bake_moon(); else bake_earth(); }

    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_fb, 466, 466, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, touch_cb, LV_EVENT_RELEASED, NULL);

    render();
    s_loop = lv_timer_create(step, 33, NULL);
    ESP_LOGI("orb", "%s 여는 데 %u ms (사진 %s)", ORB_NAME[kind],
             (unsigned)((esp_timer_get_time() - _open0) / 1000),
             s_tex_bake ? "코드로 만듦" : (s_tex_owned ? "PSRAM 으로 옮김" : "플래시에서 바로"));
    return s_loop;
}

void orb_stop(void)
{
    s_loop = NULL;                 /* 타이머는 부른 쪽이 지운다 */
    /* 🚨 그림을 먼저 지우고 버퍼를 놓는다. 거꾸로 하면 캔버스가 이미
     * 놓아버린 메모리를 가리킨 채로 남아, 지우는 도중에 그걸 읽는다
     * (0909 시뮬에서 세 번째로 열 때 그렇게 죽었다). */
    if (s_canvas) { lv_obj_delete(s_canvas); s_canvas = NULL; }
    /* PSRAM 은 꼭 돌려준다. 434KB + 260KB + 테이블이라 안 놓으면 금방 찬다. */
    if (s_fb)        { heap_caps_free(s_fb);        s_fb = NULL; }
    s_tex = NULL;
    if (s_tex_owned) { heap_caps_free(s_tex_owned); s_tex_owned = NULL; }
    if (s_lut_shade) { heap_caps_free(s_lut_shade); s_lut_shade = NULL; }
    if (s_lut_z)     { heap_caps_free(s_lut_z);     s_lut_z = NULL; }
    if (s_col_x)     { heap_caps_free(s_col_x);     s_col_x = NULL; }
    if (s_asin_tab)  { heap_caps_free(s_asin_tab);  s_asin_tab = NULL; }
    if (s_lut_tu)    { heap_caps_free(s_lut_tu);    s_lut_tu = NULL; }
    if (s_lut_tv)    { heap_caps_free(s_lut_tv);    s_lut_tv = NULL; }
    s_uv_ok = false;
    if (s_span0)     { heap_caps_free(s_span0);     s_span0 = NULL; }
    if (s_span1)     { heap_caps_free(s_span1);     s_span1 = NULL; }
}

/* 검증용 — 기울기를 각도로 바로 넣는다. 극을 정확히 정면으로 보는
 * 최악의 각도를 손으로 만들려면 여세 때문에 잘 안 맞는다. */
void orb_set_lean_deg(float deg) /* 검증용 — 축을 옆으로 기울인다 */
{
    s_lean = deg * 0.0174533f;
    s_lean_v = 0;
    s_dirty = true;
}

void orb_set_lon(float lon)      /* 검증용 — 정면에 올 경도(0~1) */
{
    s_lon = lon;
    s_dirty = true;
}

void orb_set_tilt_deg(float deg)
{
    s_tilt = deg * 0.0174533f;
    s_tilt_v = 0;
    s_dirty = true;
}

void orb_debug(float *lon, uint32_t *render_us)
{
    if (lon) *lon = s_lon;
    if (render_us) *render_us = s_render_us;
}
