/* WiFi — 배지에서 혼자 붙는다.
 *
 * 🚨 폰이나 PC 가 있어야 설정이 되는 물건은 비상용이 아니다. 그래서 AP 를
 * 열어 폰 브라우저로 받는 흔한 방식을 안 쓴다. 주변을 훑어 고르고 비밀번호를
 * 배지에서 친다(멀티탭 키패드, keypad.c).
 *
 * 🚨 처음엔 "칸 목록 → 훑기 → 고르기" 로 만들었다가 갈아엎었다(0911 지적:
 * "저장됐던 와이파이 클릭하면 거기에 접속해야지 왜 목록을 보냐").
 * **칸은 안쪽 사정이지 사람이 볼 것이 아니다.** 들어가면 바로 주변 목록이
 * 뜨고, 누르면 그냥 붙는다:
 *     저장된 것을 누르면   → 저장된 비밀번호로 곧장 붙는다
 *     처음 보는 것을 누르면 → 키패드 → 저장하고 붙는다
 *     길게 누르면          → 잊는다
 * 칸은 저장할 때 빈 곳을 알아서 고른다.
 *
 * 🚨 배지는 WiFi 에 **계속 붙어 있지 않는다** — 시각 맞추거나 올릴 때만 켰다
 * 끈다(전력). 그래서 초록은 "지금 붙어 있다" 가 아니라 **마지막으로 붙는 데
 * 성공한 망**이라는 뜻이다. 사람이 알고 싶은 것("이거 되는 망인가")과 맞다.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr, *s_list, *s_note;
static lv_timer_t *s_poll;
static char        s_pick[33];             /* 고른 SSID */
static char        s_lastok[33];           /* 마지막으로 붙은 망 */
static wifi_found_t s_found[WIFI_SCAN_MAX];
static int          s_n;

static void show_scan(void);
static void rescan_cb(lv_timer_t *t) { (void)t; show_scan(); }

/* ── 공통 ─────────────────────────────────────────────────── */
static void clear_body(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; }
    if (s_note) { lv_obj_delete(s_note); s_note = NULL; }
}

static void note(const char *txt, uint32_t col)
{
    if (!s_note) {
        s_note = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_note, &lv_font_montserrat_18, 0);
        lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_set_style_text_color(s_note, lv_color_hex(col), 0);
    lv_label_set_text(s_note, txt);
}

/* ── 붙어보는 동안 ─────────────────────────────────────────── */
static void try_poll(lv_timer_t *t)
{
    (void)t;
    int st = port_wifi_try_state();
    if (st == WIFI_TRY_BUSY) return;
    lv_timer_delete(s_poll); s_poll = NULL;

    if (st == WIFI_TRY_OK) {
        note("connected", 0x5BD48A);
    } else {
        /* 🚨 틀린 비밀번호와 신호 없음을 가릴 방법이 마땅치 않다. 둘 다 겪는
         * 일이라 둘 다 짚어준다 — 하나만 적으면 엉뚱한 데를 고치게 된다. */
        note("failed - wrong key?", 0xE06A6A);
    }
    /* 결과를 잠깐 보이고 목록으로 — 어느 줄이 초록이 됐는지 보여야 한다. */
    s_poll = lv_timer_create(rescan_cb, 1600, NULL);
    lv_timer_set_repeat_count(s_poll, 1);
}

static void try_now(const char *ssid, const char *pass)
{
    clear_body();
    note("connecting...", 0xE0B33A);
    port_wifi_try(ssid, pass);
    s_poll = lv_timer_create(try_poll, 300, NULL);
}

/* 비밀번호를 다 치면 — 저장하고 바로 붙어본다 */
static void pass_done(const char *text)
{
    if (!text) { show_scan(); return; }        /* 취소 */
    int slot = port_wifi_slot_find(s_pick);
    if (slot < 0) slot = port_wifi_slot_free();
    port_wifi_slot_set(slot, s_pick, text);
    try_now(s_pick, text);
}

/* ── 목록 ──────────────────────────────────────────────────── */
static void ap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    snprintf(s_pick, sizeof s_pick, "%s", s_found[i].ssid);
    if (s_found[i].saved) {
        /* 이미 아는 망 — 다시 칠 이유가 없다. 그냥 붙는다. */
        try_now(s_pick, NULL);
    } else {
        clear_body();
        keypad_open(s_pick, NULL, pass_done);
    }
}

/* 길게 = 잊는다 */
static void forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n || !s_found[i].saved) return;
    port_wifi_slot_clear(s_found[i].saved - 1);
    show_scan();
}

static void scan_poll(lv_timer_t *t)
{
    (void)t;
    int n = port_wifi_scan_result(s_found, WIFI_SCAN_MAX);
    if (n < 0) return;                          /* 아직 도는 중 */
    s_n = n;
    lv_timer_delete(s_poll); s_poll = NULL;
    if (s_note) { lv_obj_delete(s_note); s_note = NULL; }
    if (s_n == 0) { note("nothing around", 0x6E7686); return; }

    /* 🚨 저장된 것을 위로 올린다. 늘 쓰는 망이 열 줄 아래 있으면 굴려서
     * 찾아야 하고, 그게 "누르면 붙는다" 의 값어치를 깎는다.
     * 무리 안에서는 신호가 센 순서 그대로다(훑기가 이미 그 순서로 준다).
     * 자리를 지키는 정렬이라 안정적이다. */
    for (int i = 1; i < s_n; i++) {
        wifi_found_t k = s_found[i];
        if (!k.saved) continue;
        int j = i - 1;
        while (j >= 0 && !s_found[j].saved) { s_found[j + 1] = s_found[j]; j--; }
        s_found[j + 1] = k;
    }

    port_wifi_last_ok(s_lastok, sizeof s_lastok);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 300);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_n; i++) {
        bool ok = s_lastok[0] && strcmp(s_lastok, s_found[i].ssid) == 0;
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 62);
        lv_obj_set_style_radius(b, 16, 0);
        /* 붙는 데 성공한 망은 통째로 초록. 저장만 된 것은 한 톤 낮게. */
        lv_obj_set_style_bg_color(b, lv_color_hex(
            ok ? 0x2E6E5A : (s_found[i].saved ? 0x24343A : 0x1D1D24)), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_left(b, 18, 0);
        lv_obj_add_event_cb(b, ap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (s_found[i].saved)
            lv_obj_add_event_cb(b, forget_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *t2 = lv_label_create(b);
        lv_label_set_text(t2, s_found[i].ssid);
        lv_obj_set_style_text_font(t2, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t2, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(t2, LV_ALIGN_LEFT_MID, 0, -11);

        char sub[44];
        if (ok)                    snprintf(sub, sizeof sub, "%d dBm  -  connected", s_found[i].rssi);
        else if (s_found[i].saved) snprintf(sub, sizeof sub, "%d dBm  -  saved", s_found[i].rssi);
        else                       snprintf(sub, sizeof sub, "%d dBm", s_found[i].rssi);
        lv_obj_t *s2 = lv_label_create(b);
        lv_label_set_text(s2, sub);
        lv_obj_set_style_text_font(s2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s2, lv_color_hex(ok ? 0xCDEEDD : 0x8A93A6), 0);
        lv_obj_align(s2, LV_ALIGN_LEFT_MID, 0, 12);
    }
}

static void show_scan(void)
{
    clear_body();
    note("looking around...", 0x6E7686);
    port_wifi_scan_start();
    s_poll = lv_timer_create(scan_poll, 200, NULL);
}

/* ── 열고 닫기 ─────────────────────────────────────────────── */
static void wifi_setup_close(void)
{
    clear_body();
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
}

void wifi_setup_open(void)
{
    if (s_scr) return;
    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(s_scr);
    lv_label_set_text(t, "Wi-Fi");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -186);

    lv_obj_t *h = lv_label_create(s_scr);
    lv_label_set_text(h, "tap to join  -  hold to forget");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 186);

    /* 손잡이를 올려도, 뒤로 단추를 눌러도 나간다.
     * 🚨 손잡이만 두면 아는 사람만 쓴다 — 보이는 문이 하나 있어야 한다. */
    ui_back_btn(s_scr, wifi_setup_close);
    launcher_handle_add(s_scr, wifi_setup_close);
    show_scan();               /* 들어오면 바로 훑는다 */
}
