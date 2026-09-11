/* 설정.
 *
 * 🚨 예전엔 여섯 장을 위아래로 넘겼다. 항목이 늘면서 맨 아래 것(WiFi)이
 * 다섯 번 넘겨야 나오는 자리로 밀렸다(0911 지적). **목록을 먼저 보이고 고른
 * 것만 연다.** 무엇이 있는지 한눈에 보이고, 값도 목록에 같이 적어서
 * "보려고 들어가는" 일이 없게 했다 — 배터리가 그래서 장이 아니라 한 줄이다.
 *
 * 조작부는 여전히 화면 가장자리 링을 쓴다 — 이 화면에서 제일 큰 물건이고
 * 원형에서만 쓸 수 있는 조작이다.
 *
 * 🚨 자세한 장은 맨 위 층(lv_layer_top)에 띄운다. 목록 화면을 안 건드리고
 * 덮었다가 걷으면 그대로 돌아온다 — 목록을 다시 지을 이유가 없다.
 *
 * 🚨 '화면 여백(Edge)' 장은 뺐다. 한 번 맞추면 끝나는 값이라 자리만 차지했다.
 * 설정값 자체는 남아 있다(display.c 가 쓴다) — 다시 맞출 일이 생기면 장을
 * 되살리지 말고 값을 코드에서 바꿔라. */
#include "app.h"
#include "port.h"
#include "display.h"
#include <stdio.h>

static lv_obj_t *s_gap_lbl;

static bool s_sound_on = true;
static int  s_volume   = 60;

/* ── 저장 ────────────────────────────────────────────────────── */

typedef struct {
    int32_t bright, volume, timeout, sound_on, xgap;
} settings_t;

static void settings_save(void)
{
    settings_t v = { port_brightness_get(), s_volume, launcher_get_timeout(), s_sound_on,
                     badge_display_get_xgap() };
    port_kv_write("cfg", &v, sizeof(v));
}

static void gap_paint(void)
{
    if (s_gap_lbl) lv_label_set_text_fmt(s_gap_lbl, "x gap %d", badge_display_get_xgap());
}

/* 후보를 돌아가며 짚는다. 컨트롤러 열 수가 480이면 8, 478이면 6(그대로),
 * 476이면 4, 472면 0 이 맞는 값이다. 가장자리 색 띠가 없어지는 걸 고르면 된다. */
static void gap_cb(lv_event_t *e)
{
    (void)e;
    static const int CAND[] = { 6, 8, 4, 0, 2, 10, 12 };
    const int N = (int)(sizeof(CAND) / sizeof(CAND[0]));
    int cur = badge_display_get_xgap(), i = 0;
    for (int k = 0; k < N; k++) if (CAND[k] == cur) { i = k; break; }
    badge_display_set_xgap(CAND[(i + 1) % N]);
    gap_paint();
    settings_save();
}

void settings_load(void)
{
    settings_t v;
    if (!port_kv_read("cfg", &v, sizeof(v))) {
        /* 저장된 설정이 없으면 BSP 기본값인 100% 가 그대로 남는다.
         * 1.75인치 AMOLED 에서 100% 는 실내에선 눈부시고 전류는 밝기에
         * 그대로 비례한다. 처음부터 45% 로 내려 시작한다. */
        port_brightness_set(45);
        return;
    }
    if (v.bright >= 5 && v.bright <= 100) port_brightness_set(v.bright);
    if (v.volume >= 0 && v.volume <= 100) s_volume = v.volume;
    s_sound_on = v.sound_on != 0;
    launcher_set_timeout(v.timeout);
    port_tone_volume(s_sound_on ? s_volume : 0);
    if (v.xgap >= 0 && v.xgap <= 16) badge_display_set_xgap(v.xgap);
}

/* ── 공통 조각 ───────────────────────────────────────────────── */

static lv_obj_t *title(lv_obj_t *p, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -140);
    return l;
}

static lv_obj_t *big(lv_obj_t *p, int dy)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

/* 가장자리를 도는 굵은 링. 이 화면에서 가장 잡기 쉬운 조작부다. */
static lv_obj_t *ring(lv_obj_t *p, int lo, int hi, int val, lv_event_cb_t cb, uint32_t col)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_set_size(a, 404, 404);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, lo, hi);
    lv_arc_set_value(a, val);
    lv_obj_set_style_arc_width(a, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 26, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x1E1E24), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(a, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 12, LV_PART_KNOB);
    /* 이게 없으면 링의 사각 경계 전체가 클릭 영역이 된다 —
     * 화면 한가운데를 쓸어도 링이 먹어서 장을 못 넘긴다. */
    lv_obj_add_flag(a, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(a, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return a;
}

/* ── 장: 밝기 ───────────────────────────────────────────────── */

static lv_obj_t *s_bright_lbl;

static void bright_cb(lv_event_t *e)
{
    int v = lv_arc_get_value(lv_event_get_target(e));
    port_brightness_set(v);
    lv_label_set_text_fmt(s_bright_lbl, "%d%%", v);
    settings_save();
}

/* ── 장: 소리 ───────────────────────────────────────────────── */

static lv_obj_t *s_vol_lbl, *s_sound_btn_lbl;

static void vol_cb(lv_event_t *e)
{
    s_volume = lv_arc_get_value(lv_event_get_target(e));
    if (s_sound_on) port_tone_volume(s_volume);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_volume);
    settings_save();
}

static void sound_cb(lv_event_t *e)
{
    (void)e;
    s_sound_on = !s_sound_on;
    port_tone_volume(s_sound_on ? s_volume : 0);
    lv_label_set_text(s_sound_btn_lbl, s_sound_on ? "ON" : "OFF");
    settings_save();
}

/* ── 장: 화면 꺼짐 ──────────────────────────────────────────── */

static const int TIMEOUTS[4] = { 15, 30, 60, 0 };
static lv_obj_t *s_to_btn[4];

static lv_obj_t *s_rot_lbl;

static void rot_cb(lv_event_t *e)
{
    (void)e;
    bool on = !launcher_get_autorotate();
    launcher_set_autorotate(on);
    lv_label_set_text(s_rot_lbl, on ? "auto-rotate ON" : "auto-rotate OFF");
}

static void timeout_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    launcher_set_timeout(TIMEOUTS[i]);
    for (int k = 0; k < 4; k++)
        lv_obj_set_style_bg_color(s_to_btn[k], lv_color_hex(k == i ? 0x2E6E9E : 0x1C1C22), 0);
    settings_save();
}

/* ── 장: 시각 ───────────────────────────────────────────────── */

typedef struct { int min; const char *name; } tz_t;
static const tz_t TZS[] = {
    { -600, "Honolulu" }, { -480, "Los Angeles" }, { -420, "Denver" }, { -360, "Chicago" },
    { -300, "New York" }, { -180, "Sao Paulo" }, { 0, "London" }, { 60, "Paris" },
    { 120, "Athens" }, { 180, "Moscow" }, { 210, "Tehran" }, { 240, "Dubai" },
    { 270, "Kabul" }, { 300, "Karachi" }, { 330, "Delhi" }, { 345, "Kathmandu" },
    { 360, "Dhaka" }, { 390, "Yangon" }, { 420, "Bangkok" }, { 480, "Shanghai" },
    { 525, "Eucla" }, { 540, "Seoul" }, { 570, "Adelaide" }, { 600, "Sydney" },
    { 720, "Auckland" }, { 780, "UTC+13" }, { 840, "UTC+14" },
};
#define TZ_CNT (sizeof(TZS) / sizeof(TZS[0]))

static lv_obj_t   *s_net_lbl;
static lv_timer_t *s_poll;

static void tz_cb(lv_event_t *e)
{
    uint32_t i = lv_roller_get_selected(lv_event_get_target(e));
    if (i < TZ_CNT) port_set_tz_offset(TZS[i].min);
}

static void sync_cb(lv_event_t *e) { (void)e; port_time_sync_start(); }
/* 🚨 WiFi 설정은 설정 앱의 타일뷰 **밖**에 띄운다. 안에 넣으면 목록을
 * 위아래로 굴리는 것과 타일 넘기기가 싸운다. */
static void wifi_cb(lv_event_t *e) { (void)e; wifi_setup_open(); }

static void list_paint(void);

/* 🚨 이 함수는 **지금 화면에 있는 것만** 만져야 한다. 배터리 장을 없애면서
 * s_bat_* 를 지웠는데 여기서 계속 쓰고 있었다 — NULL 라벨에 글자를 넣어
 * 그 자리에서 멎었다(0911 시뮬). 라벨은 장이 열려 있을 때만 산다.
 * 값 표시는 목록이 맡는다(list_paint). */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    list_paint();                       /* 배터리·연결 상태는 목록에 적힌다 */

    if (!s_net_lbl || !lv_obj_is_valid(s_net_lbl)) return;   /* 시각 장이 닫혀 있다 */
    static const char *TXT[] = { "not set", "idle", "connecting", "synced", "failed" };
    static const uint32_t COL[] = { 0x666666, 0x8A8A8A, 0xE0B33A, 0x5BD48A, 0xE06A6A };
    net_state_t st = port_time_sync_state();
    lv_label_set_text(s_net_lbl, TXT[st]);
    lv_obj_set_style_text_color(s_net_lbl, lv_color_hex(COL[st]), 0);
}


/* ── 앱 ─────────────────────────────────────────────────────── */

static lv_obj_t *pill(lv_obj_t *p, const char *txt, int w, int h, int dx, int dy,
                      uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

/* ── 자세한 장 ─────────────────────────────────────────────────
 * 목록에서 고르면 이 판이 위에 뜬다. 손잡이를 올리면 걷힌다. */
static lv_obj_t *s_page;

static void page_close(void)
{
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    list_paint();                 /* 바꾼 값이 목록에 바로 보이게 */
}

static lv_obj_t *page_open(const char *name)
{
    if (s_page) return NULL;
    s_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, 466, 466);
    lv_obj_center(s_page);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_CLICKABLE);   /* 뒤로 안 새게 */
    title(s_page, name);
    ui_back_btn(s_page, page_close);
    launcher_handle_add(s_page, page_close);
    return s_page;
}

/* ── 장들 ──────────────────────────────────────────────────── */
static void open_bright(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Brightness");
    if (!p) return;
    s_bright_lbl = big(p, 0);
    int b = port_brightness_get();
    lv_label_set_text_fmt(s_bright_lbl, "%d%%", b);
    ring(p, 5, 100, b, bright_cb, 0x7FB0FF);
}

static void open_sound(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Sound");
    if (!p) return;
    s_vol_lbl = big(p, -26);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", s_volume);
    ring(p, 0, 100, s_volume, vol_cb, 0x5BD48A);
    lv_obj_t *sb = pill(p, s_sound_on ? "ON" : "OFF", 140, 60, 0, 66,
                        0x1C1C22, sound_cb, NULL);
    s_sound_btn_lbl = lv_obj_get_child(sb, 0);
}

static void open_screen(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Screen off");
    if (!p) return;
    const char *TO_TXT[4] = { "15s", "30s", "60s", "never" };
    int cur = launcher_get_timeout();
    for (int i = 0; i < 4; i++) {
        s_to_btn[i] = pill(p, TO_TXT[i], 150, 68,
                           (i % 2) ? 82 : -82, (i / 2) ? 48 : -32,
                           TIMEOUTS[i] == cur ? 0x2E6E9E : 0x1C1C22,
                           timeout_cb, (void *)(intptr_t)i);
    }
    lv_obj_t *rb = pill(p, launcher_get_autorotate() ? "auto-rotate ON" : "auto-rotate OFF",
                        280, 54, 0, 136, 0x1C1C22, rot_cb, NULL);
    s_rot_lbl = lv_obj_get_child(rb, 0);
}

static void open_time(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p = page_open("Time");
    if (!p) return;
    lv_obj_t *rl = lv_roller_create(p);
    char opts[TZ_CNT * 14];
    int n = 0, sel = 0, cur_tz = port_get_tz_offset();
    for (unsigned i = 0; i < TZ_CNT; i++) {
        n += snprintf(opts + n, sizeof(opts) - n, "%s%s", i ? "\n" : "", TZS[i].name);
        if (TZS[i].min == cur_tz) sel = i;
    }
    lv_roller_set_options(rl, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(rl, 3);
    lv_obj_set_width(rl, 300);
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_bg_color(rl, lv_color_hex(0x141418), 0);
    lv_obj_set_style_border_width(rl, 0, 0);
    lv_obj_set_style_radius(rl, 22, 0);
    lv_obj_set_style_bg_color(rl, lv_color_hex(0x2E4A66), LV_PART_SELECTED);
    lv_roller_set_selected(rl, sel, LV_ANIM_OFF);
    lv_obj_add_event_cb(rl, tz_cb, LV_EVENT_VALUE_CHANGED, NULL);

    pill(p, "sync now", 220, 62, 0, 74, 0x1C1C22, sync_cb, NULL);
    s_net_lbl = lv_label_create(p);
    lv_obj_set_style_text_font(s_net_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_net_lbl, LV_ALIGN_CENTER, 0, 136);
    poll_cb(NULL);
}

static void open_hosts(lv_event_t *e)
{
    /* 🚨 호스트 목록도 제 화면을 스스로 띄운다 — page_open 을 쓰면 두 겹이 된다. */
    (void)e;
    host_pick_open();
}

static void open_wifi(lv_event_t *e)
{
    /* 🚨 WiFi 는 제 화면을 스스로 띄운다(wifi_setup.c). 여기서 page_open 을
     * 쓰면 판이 두 겹이 돼 손잡이가 엉킨다. */
    (void)e;
    wifi_setup_open();
}

/* ── 목록 ──────────────────────────────────────────────────── */
static lv_obj_t *s_list;
static lv_obj_t *s_sub[7];          /* 줄마다 값 글자 */

static lv_obj_t *menu_row(lv_obj_t *parent, const char *name, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 340, 62);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(cb ? 0x1D1D24 : 0x141418), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 18, 0);
    lv_obj_set_style_pad_right(b, 18, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    else    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, name);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(cb ? 0xE8ECF0 : 0x9AA4AE), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(b);
    lv_label_set_text(v, "");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;                        /* 값 글자를 돌려준다 */
}

/* 🚨 목록에 값을 같이 적는 게 요점이다 — 값만 보려고 장을 여는 일이 없게. */
static void list_paint(void)
{
    if (!s_sub[0]) return;
    lv_label_set_text_fmt(s_sub[0], "%d%%", port_brightness_get());
    lv_label_set_text_fmt(s_sub[1], "%d%%  %s", s_volume, s_sound_on ? "on" : "off");

    int to = launcher_get_timeout();
    if (to <= 0) lv_label_set_text(s_sub[2], "never");
    else         lv_label_set_text_fmt(s_sub[2], "%ds", to);

    static const char *NET[] = { "not set", "idle", "connecting", "synced", "failed" };
    int tz = port_get_tz_offset();
    const char *tzn = "";
    for (unsigned i = 0; i < TZ_CNT; i++) if (TZS[i].min == tz) tzn = TZS[i].name;
    lv_label_set_text_fmt(s_sub[3], "%s  %s", tzn, NET[port_time_sync_state()]);

    int saved = 0;
    for (int i = 0; i < WIFI_SLOTS; i++) {
        char ss[33];
        if (port_wifi_slot_get(i, ss, sizeof ss)) saved++;
    }
    lv_label_set_text_fmt(s_sub[4], "%d saved", saved);

    hid_host_t hh[HID_HOSTS_MAX];
    int nh = port_hid_hosts(hh, HID_HOSTS_MAX);
    lv_label_set_text_fmt(s_sub[5], "%d paired", nh);

    /* 배터리는 장을 안 만든다 — 값을 보려고 한 번 더 누를 이유가 없다. */
    int p = port_battery_percent();
    if (p < 0) {
        lv_label_set_text(s_sub[6], "--");
    } else if (port_battery_plugged()) {
        lv_label_set_text_fmt(s_sub[6], "%d%%  charging", p);
    } else {
        int m = port_battery_minutes_left();
        if (m < 0) lv_label_set_text_fmt(s_sub[6], "%d%%  measuring", p);
        else       lv_label_set_text_fmt(s_sub[6], "%d%%  %dh %02dm", p, m / 60, m % 60);
    }
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_SETTINGS);
    s_page = NULL;
    s_net_lbl = NULL;

    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, "Settings");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -190);

    /* 🚨 둥근 화면이라 목록은 가운데를 넓게 쓰고 위아래로 굴린다. 줄 높이를
     * 62px 로 크게 둬서 굴리다 잘못 눌리는 일을 줄인다. */
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 330);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_sub[0] = menu_row(s_list, "Brightness", open_bright);
    s_sub[1] = menu_row(s_list, "Sound",      open_sound);
    s_sub[2] = menu_row(s_list, "Screen off", open_screen);
    s_sub[3] = menu_row(s_list, "Time",       open_time);
    s_sub[4] = menu_row(s_list, "Wi-Fi",      open_wifi);
    s_sub[5] = menu_row(s_list, "Bluetooth",  open_hosts);
    s_sub[6] = menu_row(s_list, "Battery",    NULL);   /* 정보만 */

    list_paint();
    s_poll = lv_timer_create(poll_cb, 5000, NULL);   /* 배터리는 몇 분에 한 칸 */
}

static void leave(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    /* 🚨 자세한 장은 맨 위 층에 있어서 앱 화면과 같이 안 지워진다.
     * 안 걷으면 홈으로 나가도 설정 장이 화면을 덮은 채 남는다. */
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_list = NULL;
    for (int i = 0; i < 7; i++) s_sub[i] = NULL;
    s_net_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x9AA0A6); }

const badge_app_t app_settings = {
    .name = "Settings", .icon = LV_SYMBOL_SETTINGS, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
