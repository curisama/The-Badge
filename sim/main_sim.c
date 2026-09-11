/* LVGL 시뮬레이터.
 *   --shots        스크린샷 몇 장 찍고 끝 (기본)
 *   --serve        stdin 명령을 받아 프레임을 stdout 으로 뱉는 대화형 모드
 * 실기와 같은 UI 코드를 PC에서 돌린다. */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
/* 🚨 윈도우 stdio 는 기본이 글자 모드다. 프레임 안의 0x0A 가 0x0D0A 로 불어나
 * 화면 데이터가 조용히 망가진다 — 파이프를 날것 모드로 돌려놔야 한다. */
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#define BADGE_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define BADGE_MKDIR(p) mkdir((p), 0755)
#endif

#define W 466
#define H 466

static uint16_t s_fb[W * H];

extern uint32_t g_sim_us;                     /* port_sim.c 의 가상 시계 */
static uint32_t tick_cb(void) { return g_sim_us / 1000; }


/* ── 화면 ────────────────────────────────────────────────────── */

/* ── 그리기 양 계측 ──────────────────────────────────────────
 * 🚨 이 화면은 QSPI 40MHz x 4선 = 초당 20MB 가 천장이다. 한 프레임을
 * 통째로 밀면 466x466x2 = 434KB 라 21.7ms, 이론상 46fps 다.
 * 실기에서 잰 값(전체 화면급 부하에서 20ms 타이머가 58ms 로 밀렸다)을
 * 생각하면 실제 천장은 그보다 낮다.
 *
 * 시뮬은 무한히 빠른 PC 라 "되는 것처럼" 보인다 — 그래서 굽고 나서야
 * 안 되는 걸 안다. 여기서 바이트를 세어 예산을 넘는지 미리 알려준다. */
#define QSPI_BYTES_PER_SEC  (20u * 1000u * 1000u)
static uint64_t g_draw_bytes;      /* 지금까지 민 총 바이트 */
static uint32_t g_draw_calls;
static uint64_t g_win_bytes;       /* 측정 창 안에서 민 바이트 */
static uint32_t g_win_start_ms;
static uint32_t g_win_frames;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    g_draw_bytes += (uint64_t)w * h * 2;
    g_win_bytes  += (uint64_t)w * h * 2;
    g_draw_calls++;

    const uint16_t *src = (const uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; y++)
        for (int x = area->x1; x <= area->x2; x++)
            s_fb[y * W + x] = *src++;
    lv_display_flush_ready(disp);
}

/* ── 터치 ────────────────────────────────────────────────────── */

static int32_t s_touch_x, s_touch_y;
static bool    s_touch_down;
static bool    s_pending_press;   /* 눌렀다 바로 뗀 탭을 놓치지 않기 위한 자국 */

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;

    bool pressed = s_touch_down || s_pending_press;
    if (s_pending_press && !s_touch_down) s_pending_press = false;  /* 한 번 보고하면 소진 */
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ── 시간 진행 ───────────────────────────────────────────────── */

/* 다마고치가 떠 있으면 에뮬 CPU가 시계를 민다. 아니면 우리가 민다. */
/* ── 시계 흔들기 ─────────────────────────────────────────────
 * 🚨 시뮬은 시간이 자로 잰 듯 고르게 흐른다. 실기는 안 그렇다 —
 * 0908 실측에서 20ms 타이머가 게임 중엔 20ms 였지만 화면이 바쁠 땐
 * 평균 57ms, 최대 251ms 로 밀렸다. 브레이크아웃 공이 들쭉날쭉했던 게
 * 그것 때문이었는데, 시뮬에선 고르게 흘러서 끝까지 안 보였다.
 * 흔들어 놓으면 그런 버그가 시뮬에서 드러난다. 'J n' 으로 켠다. */
static uint32_t g_jitter_max;      /* 0 = 안 흔든다 */
static uint32_t g_jit_seed = 2463534242u;

static uint32_t jit_rnd(void)
{
    g_jit_seed ^= g_jit_seed << 13;
    g_jit_seed ^= g_jit_seed >> 17;
    g_jit_seed ^= g_jit_seed << 5;
    return g_jit_seed;
}

static void advance(uint32_t us)
{
    const uint32_t BASE = 5000;
    for (uint32_t done = 0; done < us; ) {
        uint32_t chunk = BASE;
        if (g_jitter_max) {
            /* 대개는 짧게, 이따금 크게 — 실기의 "가끔 확 밀림"을 흉내낸다 */
            uint32_t r = jit_rnd() % 100;
            chunk = (r < 80) ? BASE
                  : (r < 97) ? BASE * 4
                             : g_jitter_max * 1000;
        }
        if (done + chunk > us) chunk = us - done;
        g_sim_us += chunk;
        done += chunk;
        lv_timer_handler();
    }
}

/* ── PNG 저장 (스크린샷 모드) ────────────────────────────────── */

/* 🚨 예전엔 /tmp 에 PPM 을 쓰고 `mkdir -p shots && pnmtopng` 를 불렀다.
 * 윈도우엔 셋 다 없다 — /tmp 도, netpbm 도, `mkdir -p` 를 아는 셸도. 그래서
 * 스크린샷이 죄다 0바이트로 나왔는데 프로그램은 성공했다며 끝났다(0909).
 * 이제 밖을 아무것도 안 부르고 직접 쓴다. 눌러 담지 않아 한 장에 650KB 쯤
 * 되지만 sim/shots 는 깃에 안 올라가니 상관없다. */

static uint32_t s_crc_tab[256];
static int s_crc_ready;

static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n)
{
    if (!s_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            s_crc_tab[i] = v;
        }
        s_crc_ready = 1;
    }
    for (size_t i = 0; i < n; i++) c = s_crc_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t buf[4];
    put_be32(buf, (uint32_t)n);
    fwrite(buf, 1, 4, f);
    fwrite(tag, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc_upd(0xFFFFFFFFu, (const uint8_t *)tag, 4);
    if (n) crc = crc_upd(crc, data, n);
    put_be32(buf, crc ^ 0xFFFFFFFFu);
    fwrite(buf, 1, 4, f);
}

static void write_png(const char *name, int mask_corners)
{
    char png[256];
    BADGE_MKDIR("shots");
    snprintf(png, sizeof(png), "shots/%s.png", name);

    /* PNG 는 줄마다 앞에 거르개 종류 한 바이트가 붙는다(0 = 안 쓴다). */
    const size_t stride = 1 + (size_t)W * 3;
    const size_t raw_n = stride * (size_t)H;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) return;

    const int cx = W / 2, cy = H / 2, r2 = (W / 2) * (W / 2);
    for (int y = 0; y < H; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        *row++ = 0;
        for (int x = 0; x < W; x++) {
            uint16_t c = s_fb[y * W + x];
            uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
            int dx = x - cx, dy = y - cy;
            if (mask_corners && dx * dx + dy * dy > r2) r = g = b = 40;
            *row++ = r; *row++ = g; *row++ = b;
        }
    }

    /* zlib 껍데기 + "안 눌러 담은" 블록들. 블록 하나에 65535바이트까지. */
    const size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = (uint8_t *)malloc(2 + blocks * 5 + raw_n + 4);
    if (!z) { free(raw); return; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = (raw_n - off < 65535) ? raw_n - off : 65535;
        z[zi++] = (off + n >= raw_n) ? 1 : 0;              /* 마지막 블록 표시 */
        z[zi++] = (uint8_t)(n & 0xFF);
        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);
        z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
    }
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_n; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    put_be32(z + zi, (s2 << 16) | s1);
    zi += 4;

    FILE *f = fopen(png, "wb");
    if (!f) { free(raw); free(z); return; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)W);
    put_be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8;                       /* 한 칸 8비트 */
    ihdr[9] = 2;                       /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    fprintf(stderr, "찍음: %s\n", png);
}

/* ── 대화형 모드 ─────────────────────────────────────────────
 * 한 줄 명령을 받는다:
 *   T <x> <y> <0|1>   터치
 *   H                 홈 버튼 (BOOT)
 *   N <n>             지금 닿아 있는 손가락 수
 *   W                 PWR 짧게 누름 (화면 토글)
 *   P <ms>            그 시간만큼 진행
 *   F                 프레임 요청 → "FRAME <바이트수>\n" + RGB888 원본
 *   R                 프레임 요청 → "FRAME <바이트수>\n" + RGB565 원본 (절반 크기)
 *   Q                 종료 */
static void serve_loop(void)
{
    char line[128];
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    while (fgets(line, sizeof(line), stdin)) {
        switch (line[0]) {
        case 'T': {
            int x, y, s;
            if (sscanf(line + 1, "%d %d %d", &x, &y, &s) == 3) {
                s_touch_x = x; s_touch_y = y; s_touch_down = s;
                if (s) s_pending_press = true;
                /* 여기서 시간을 밀지 않는다. 밀면 이벤트 하나마다 20ms 씩
                 * 가상 시계가 튀어서, 손가락 한 번 끌 때 시계가 몇 초씩 앞서간다. */
            }
            break;
        }
        /* 홈으로 곧장. 'H'(launcher_home) 는 잠금화면에선 홈으로 안 간다 —
         * 쪽 넘기기 같은 걸 시험하려면 확실한 문이 필요하다. */
        case 'G':
            launcher_show_home();
            break;
        case 'H':
            launcher_home();
            break;
        /* 앱을 번호로 바로 연다. 터치 좌표를 몰라도 검증을 돌릴 수 있다. */
        /* 자동 꺼짐 시간을 바꾼다(0 = 안 꺼짐). 시간을 성큼 미는 검증에 필요하다. */
        case 'K':
            launcher_set_timeout(atoi(line + 1));
            break;
        /* 공 좌표를 그대로 뱉는다. 속도가 균일한지 재려면 필요하다. */
        /* 그리기 예산 — 'D 0' 재기 시작, 'D 1' 결과. 초당 몇 MB 를 밀었고
         * 그게 QSPI 천장(20MB/s)의 몇 %인지, 그 속도로 몇 fps 가 되는지 준다. */
        /* 시계를 흔든다. 'J 0' 끔, 'J 250' = 이따금 250ms 까지 밀림 */
        case 'J':
            g_jitter_max = (uint32_t)atoi(line + 1);
            printf("JITTER %u\n", g_jitter_max);
            fflush(stdout);
            break;
        case 'D': {
            int op = atoi(line + 1);
            if (op == 0) {
                g_win_bytes = 0; g_win_frames = 0; g_win_start_ms = g_sim_us / 1000;
                printf("DRAW reset\n");
            } else {
                uint32_t ms = (g_sim_us / 1000) - g_win_start_ms;
                if (!ms) ms = 1;
                double bps  = (double)g_win_bytes * 1000.0 / ms;
                double pct  = bps / QSPI_BYTES_PER_SEC * 100.0;
                /* 이 그림을 계속 그린다면 QSPI 만으로 몇 fps 가 한계인가 */
                double per_frame = g_win_frames ? (double)g_win_bytes / g_win_frames : 0;
                double fps_cap = per_frame > 0 ? QSPI_BYTES_PER_SEC / per_frame : 0;
                printf("DRAW ms=%u bytes=%llu frames=%u bps=%.0f pct=%.1f perframe=%.0f fpscap=%.1f\n",
                       ms, (unsigned long long)g_win_bytes, g_win_frames, bps, pct, per_frame, fps_cap);
            }
            fflush(stdout);
            break;
        }
        /* 천체 기울기를 각도로 바로 넣는다(검증용) */
        /* 한 프레임 그리는 데 걸린 시간(마이크로초) — 전력 재는 창구 */
        case 'Z': {
            extern void orb_debug(float *lon, uint32_t *render_us);
            float lon = 0; uint32_t us = 0;
            orb_debug(&lon, &us);
            printf("Z %.4f %u\n", lon, (unsigned)us);
            fflush(stdout);
            break;
        }
        /* 가짜 IMU 기울기 — "I x y z" (mg). 인자가 없으면 IMU 없는 기기로 */
        case 'I': {
            extern void sim_imu_set(float x, float y, float z);
            extern void sim_imu_off(void);
            float ix, iy, iz;
            if (sscanf(line + 1, "%f %f %f", &ix, &iy, &iz) == 3) sim_imu_set(ix, iy, iz);
            else sim_imu_off();
            break;
        }
        /* 가짜 자이로 — "X gx gy gz" (dps). 인자가 없으면 자이로 없는 기기로 */
        case 'X': {
            extern void sim_gyro_set(float x, float y, float z);
            extern void sim_gyro_off(void);
            float rx, ry, rz;
            if (sscanf(line + 1, "%f %f %f", &rx, &ry, &rz) == 3) sim_gyro_set(rx, ry, rz);
            else sim_gyro_off();
            break;
        }
        case 'Y': {
            extern void orb_set_lean_deg(float deg);
            orb_set_lean_deg((float)atof(line + 1));
            break;
        }
        case 'O': {
            extern void orb_set_tilt_deg(float deg);
            orb_set_tilt_deg((float)atof(line + 1));
            break;
        }
        /* 물 상태 — 기울기 · 배 위치 · 물의 총량(보존되나 보려고) */
        case 'W' + 128: break;
        case 'V': {
            extern void water_debug(float *deg, float *boat, float *volume);
            float d = 0, b = 0, v = 0;
            water_debug(&d, &b, &v);
            printf("WATER %.2f %.1f %.1f\n", d, b, v);
            fflush(stdout);
            break;
        }
        case 'L': {
            extern void orb_set_lon(float lon);
            orb_set_lon((float)atof(line + 1));
            break;
        }
        case 'M': {
            extern void mz_debug_ball(float *x, float *y);
            float mx = 0, my = 0;
            mz_debug_ball(&mx, &my);
            printf("MARBLE %.3f %.3f\n", mx, my);
            fflush(stdout);
            break;
        }
        case 'C': {
            extern void games_debug_play_pinball(void);
            games_debug_play_pinball();
            break;
        }
        case 'S': {
            extern void pb_debug(float *, float *, int *, int *);
            float x = 0, y = 0; int pts = 0, left = 0;
            pb_debug(&x, &y, &pts, &left);
            printf("PB %.2f %.2f %d %d\n", x, y, pts, left);
            fflush(stdout);
            break;
        }
        case 'E': {
            extern void games_debug_brk_level(int lv);
            games_debug_brk_level(atoi(line + 1));
            break;
        }
        case 'U': {
            extern void games_debug_brk_info(int *, int *, int *, int *, float *, float *);
            int lv = 0, n = 0, left = 0, tough = 0; float ph = 0, ob = 0;
            games_debug_brk_info(&lv, &n, &left, &tough, &ph, &ob);
            printf("BRK %d %d %d %d %.1f %.2f\n", lv, n, left, tough, ph, ob);
            fflush(stdout);
            break;
        }
        case 'B': {
            extern void brk_debug_ball(float *x, float *y);
            float bx = 0, by = 0;
            brk_debug_ball(&bx, &by);
            printf("BALL %.3f %.3f\n", bx, by);
            fflush(stdout);
            break;
        }
        case 'A': {
            /* 🚨 물과 천체가 빠져 있었다. 둘 다 Games 목록 안에 있어서 좌표로
             * 두드려야 했는데, 그 목록은 셋만 보이고 아래로 굴려야 나온다 —
             * sim-exit-check 가 "Water 통과" 라고 찍던 것이 실은 Marble 을
             * 두드린 것이었다(0909). 홈에서 바로 여는 길이 app.h 에 이미
             * 있으니 여기에도 둔다. A 6 = 물, A 7 = 천체. */
            /* 🚨 설정도 넣는다. 홈 목록에서 좌표로 두드려야 했는데 그 목록은
             * 굴려야 나와서, 검사가 엉뚱한 앱을 열고도 통과했다고 찍혔다
             * (0909 에 물·천체가 같은 이유로 들어왔다). A 8 = 설정. */
            static const badge_app_t *const list[] = {
                &app_mouse, &app_meet, &app_keys, &app_calc, &app_games, &app_clock,
                &app_water, &app_orb, &app_settings,
            };
            int n = atoi(line + 1);
            if (n >= 0 && n < (int)(sizeof(list) / sizeof(list[0]))) launcher_open(list[n]);
            break;
        }
        case 'W':
            launcher_screen_toggle();
            break;
        case 'N': {
            extern int g_touch_count;
            int n = atoi(line + 1);
            if (n >= 0 && n <= 2) g_touch_count = n;
            break;
        }
        case 'P': {
            int ms = atoi(line + 1);
            if (ms > 0 && ms < 5000) advance((uint32_t)ms * 1000);
            break;
        }
        case 'F': {
            static uint8_t rgb[W * H * 3];
            for (int i = 0; i < W * H; i++) {
                uint16_t c = s_fb[i];
                rgb[i * 3 + 0] = ((c >> 11) & 0x1F) * 255 / 31;
                rgb[i * 3 + 1] = ((c >> 5)  & 0x3F) * 255 / 63;
                rgb[i * 3 + 2] = ( c        & 0x1F) * 255 / 31;
            }
            printf("FRAME %d\n", W * H * 3);
            fwrite(rgb, 1, sizeof(rgb), stdout);
            fflush(stdout);
            break;
        }
        case 'R': {
            g_win_frames++;
            /* RGB565 그대로 넘긴다. 변환도 없고 바이트도 절반이다. */
            printf("FRAME %d\n", W * H * 2);
            fwrite(s_fb, 1, sizeof(s_fb), stdout);
            fflush(stdout);
            break;
        }
        case 'Q':
            return;
        }
    }
}

int main(int argc, char **argv)
{
    bool serve = (argc > 1 && strcmp(argv[1], "--serve") == 0);

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(W, H);
    static uint8_t draw_buf[W * H * 2];
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);
    /* 기본 30ms 는 브라우저 너머에서 만지면 한 박자 늦게 느껴진다. */
    lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    splash_show();          /* 실기와 같은 경로로 — 스플래시가 런처를 띄운다 */
    advance(300000);
    if (!serve) write_png("00_splash", 1);   /* 서버 모드에선 stdout 이 프레임 전용이다 */
    advance(1500000);
    /* 스크린샷 모드는 가상 시계를 성큼성큼 밀기 때문에 늘 무동작 상태가 된다.
     * 자동 꺼짐을 꺼두지 않으면 검은 화면만 찍힌다. */
    if (!serve) launcher_set_timeout(0);
    if (!serve) { advance(200000); write_png("10_lock", 1); launcher_show_home(); advance(400000); }

    if (serve) {
        serve_loop();
        return 0;
    }

    write_png("01_home", 1);
    struct { const badge_app_t *app; const char *shot; } scenes[] = {
        { &app_mouse, "02_mouse" },
        { &app_clock, "04_clock" },
        { &app_meet,  "05_meet" },   /* 한글이 나오는 유일한 화면 — 폰트 확인용 */
        { &app_settings, "06_settings" },
        { &app_keys, "08_keys" },
        { &app_calc, "09_calc" },
        { &app_games, "11_games" },
    };
    for (unsigned i = 0; i < sizeof(scenes) / sizeof(scenes[0]); i++) {
        launcher_open(scenes[i].app);
        advance(1200000);
        write_png(scenes[i].shot, 1);
        launcher_home();
        advance(400000);   /* 닫는 애니메이션이 끝날 때까지 */
    }

    advance(300000);        /* 여는 애니메이션 */
    advance(50000);
    launcher_home();
    fprintf(stderr, "완료\n");
    return 0;
}
