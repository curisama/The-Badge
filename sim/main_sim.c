/* The LVGL simulator.
 *   --shots        take a few screenshots and stop (the default)
 *   --serve        interactive: take commands on stdin, spit frames to stdout
 * It runs the same UI code as the hardware, on a PC. */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
/* 🚨 Windows stdio is in text mode by default. A 0x0A inside a frame swells to
 * 0x0D0A and quietly corrupts the screen data — the pipe has to be put into
 * raw mode. */
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

extern uint32_t g_sim_us;                     /* the virtual clock in port_sim.c */
static uint32_t tick_cb(void) { return g_sim_us / 1000; }


/* ── the screen ──────────────────────────────────────────────── */

/* ── measuring how much is drawn ─────────────────────────────
 * 🚨 This screen's ceiling is QSPI at 40 MHz over 4 lines = 20 MB a second.
 * Pushing a whole frame is 466x466x2 = 434 KB, so 21.7 ms, in theory 46 fps.
 * Measured on the hardware (a 20 ms timer slipping to 58 ms under full-screen
 * load), the real ceiling is lower than that.
 *
 * The simulator is an infinitely fast PC, so everything "looks fine" — and the
 * problem only turns up after flashing. Counting the bytes here says in
 * advance whether the budget is blown. */
#define QSPI_BYTES_PER_SEC  (20u * 1000u * 1000u)
static uint64_t g_draw_bytes;      /* total bytes pushed so far */
static uint32_t g_draw_calls;
static uint64_t g_win_bytes;       /* bytes pushed inside the measuring window */
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

/* ── touch ───────────────────────────────────────────────────── */

static int32_t s_touch_x, s_touch_y;
static bool    s_touch_down;
static bool    s_pending_press;   /* a mark so a press-and-release tap is not missed */

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;

    bool pressed = s_touch_down || s_pending_press;
    if (s_pending_press && !s_touch_down) s_pending_press = false;  /* spent once reported */
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ── advancing time ──────────────────────────────────────────── */

/* With Tamagotchi up, the emulated CPU drives the clock. Otherwise we do. */
/* ── shaking the clock ───────────────────────────────────────
 * 🚨 In the simulator time flows as evenly as a ruler. On the hardware it does
 * not — measured 09-08, a 20 ms timer was 20 ms during a game but averaged
 * 57 ms and peaked at 251 ms when the screen was busy. That is what made the
 * breakout ball jerky, and the even flow in the simulator hid it all the way
 * through. Shaking the clock brings bugs like that out here. Turn it on with 'J n'. */
static uint32_t g_jitter_max;      /* 0 = no shaking */
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
            /* Mostly small, occasionally large — imitating the hardware's "sudden big slip" */
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

/* ── saving a PNG (screenshot mode) ─────────────────────────── */

/* 🚨 It used to write a PPM to /tmp and call `mkdir -p shots && pnmtopng`.
 * Windows has none of the three — no /tmp, no netpbm, no shell that knows
 * `mkdir -p`. So every screenshot came out 0 bytes while the program reported
 * success (09-09). Now it calls nothing outside and writes them itself. There
 * is no compression, so a shot is around 650 KB, but sim/shots is not in git
 * and it does not matter. */

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

    /* A PNG puts one filter-type byte in front of every row (0 = none). */
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

    /* The zlib wrapper plus "stored" blocks. One block holds up to 65535 bytes. */
    const size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = (uint8_t *)malloc(2 + blocks * 5 + raw_n + 4);
    if (!z) { free(raw); return; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = (raw_n - off < 65535) ? raw_n - off : 65535;
        z[zi++] = (off + n >= raw_n) ? 1 : 0;              /* marks the final block */
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
    ihdr[8] = 8;                       /* 8 bits a channel */
    ihdr[9] = 2;                       /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    fprintf(stderr, "shot: %s\n", png);
}

/* ── interactive mode ────────────────────────────────────────
 * It takes one command per line:
 *   T <x> <y> <0|1>   touch
 *   H                 home button (BOOT)
 *   N <n>             how many fingers are down right now
 *   W                 short PWR press (toggles the screen)
 *   P <ms>            advance by that much
 *   F                 ask for a frame → "FRAME <bytes>\n" + raw RGB888
 *   R                 ask for a frame → "FRAME <bytes>\n" + raw RGB565 (half the size)
 *   Q                 quit */
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
                /* Time is not advanced here. Advancing would jump the virtual
                 * clock 20 ms per event, so one finger drag would run it
                 * seconds ahead. */
            }
            break;
        }
        /* Straight home. 'H' (launcher_home) does not go home from the lock
         * screen — trying things like page turns needs a door that always works. */
        case 'G':
            launcher_show_home();
            break;
        case 'H':
            launcher_home();
            break;
        /* Opens an app by number. Checks can run without knowing touch coordinates. */
        /* Changes the auto-off time (0 = never). Needed by checks that jump time forward. */
        case 'K':
            launcher_set_timeout(atoi(line + 1));
            break;
        /* Spits the ball coordinates out as they are. Needed to measure whether the speed is even. */
        /* The drawing budget — 'D 0' starts measuring, 'D 1' gives the result: how
         * many MB a second were pushed, what percentage of the QSPI ceiling
         * (20 MB/s) that is, and what fps it comes to. */
        /* Shakes the clock. 'J 0' off, 'J 250' = occasional slips up to 250 ms */
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
                /* If this picture kept being drawn, what fps would QSPI alone allow */
                double per_frame = g_win_frames ? (double)g_win_bytes / g_win_frames : 0;
                double fps_cap = per_frame > 0 ? QSPI_BYTES_PER_SEC / per_frame : 0;
                printf("DRAW ms=%u bytes=%llu frames=%u bps=%.0f pct=%.1f perframe=%.0f fpscap=%.1f\n",
                       ms, (unsigned long long)g_win_bytes, g_win_frames, bps, pct, per_frame, fps_cap);
            }
            fflush(stdout);
            break;
        }
        /* Sets the orb tilt directly in degrees (for checking) */
        /* How long one frame took to draw, in microseconds — the window for measuring power */
        case 'Z': {
            extern void orb_debug(float *lon, uint32_t *render_us);
            float lon = 0; uint32_t us = 0;
            orb_debug(&lon, &us);
            printf("Z %.4f %u\n", lon, (unsigned)us);
            fflush(stdout);
            break;
        }
        /* Fake IMU tilt — "I x y z" (mg). No arguments means a device with no IMU */
        case 'I': {
            extern void sim_imu_set(float x, float y, float z);
            extern void sim_imu_off(void);
            float ix, iy, iz;
            if (sscanf(line + 1, "%f %f %f", &ix, &iy, &iz) == 3) sim_imu_set(ix, iy, iz);
            else sim_imu_off();
            break;
        }
        /* Fake gyro — "X gx gy gz" (dps). No arguments means a device with no gyro */
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
        /* Water state — tilt, boat position, total volume (to see whether it is conserved) */
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
            /* 🚨 Water and the orbs were missing. Both sit inside the Games
             * list and had to be reached by tapping coordinates, but that list
             * shows only three and the rest need scrolling — so what
             * sim-exit-check reported as "Water passed" was actually tapping
             * Marble (09-09). app.h already has a way to open them straight
             * from home, so it goes here too. A 6 = water, A 7 = orbs. */
            /* 🚨 Settings goes in as well. It had to be tapped by coordinate in
             * the home list, and that list needs scrolling, so the check opened
             * the wrong app and still reported a pass (water and the orbs came
             * in for the same reason on 09-09). A 8 = settings. */
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
            /* RGB565 passed straight through. No conversion, and half the bytes. */
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
    /* The default 30 ms feels a beat late when it is being touched from the far side of a browser. */
    lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    splash_show();          /* the same path as the hardware — the splash brings the launcher up */
    advance(300000);
    if (!serve) write_png("00_splash", 1);   /* in serve mode stdout carries frames and nothing else */
    advance(1500000);
    /* Screenshot mode strides the virtual clock forward, so it is always idle.
     * Without turning auto-off off, all it captures is a black screen. */
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
        { &app_meet,  "05_meet" },   /* the only screen with Korean on it — for checking the font */
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
        advance(400000);   /* until the closing animation has finished */
    }

        advance(300000);        /* the opening animation */
    advance(50000);
    launcher_home();
    fprintf(stderr, "done\n");
    return 0;
}
