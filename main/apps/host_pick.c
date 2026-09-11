/* 붙은 적 있는 호스트를 골라 옮겨 붙는다.
 *
 * 🚨 BLE 본딩은 **주소만** 저장한다. 호스트 이름은 안 온다 — 배지가 주변장치
 * 쪽이라 상대 이름을 볼 일이 없다. 그래서 목록을 만들면 `A4:83:E7:11:22:33`
 * 같은 게 세 줄 뜨고 어느 게 집 PC 인지 알 수가 없다. **키패드가 생겨서**
 * 사람이 이름을 지어 붙일 수 있게 됐고, 그래서 이 화면이 쓸모가 생겼다.
 *
 * 🚨 제일 까다로운 건 이전 호스트의 재연결 경합이다. 화이트리스트만으로는
 * 스택 구현에 따라 새므로, 붙는 순간 주소를 보고 아니면 끊는 방어가
 * hid_mouse.c 쪽에 하나 더 있다.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr, *s_list;
static lv_timer_t *s_poll;
static hid_host_t  s_hosts[HID_HOSTS_MAX];
static int         s_n;
static int         s_editing = -1;
static bool        s_built;            /* s_hosts 가 지금 그려진 것과 같나 */

static void show_list(void);

/* 🚨 **누르고 있는 동안 화면을 다시 지으면 그 누름은 죽는다.**
 * LVGL 은 눌린 객체가 지워지면 `lv_indev_wait_release()` 를 불러 **손을 뗄
 * 때까지 그 입력장치를 아예 무시한다**(lv_obj_tree.c). 새 객체가 같은 자리에
 * 생겨도 PRESSED 가 다시 안 간다. 그래서 길게 누르기가 영영 안 걸린다 —
 * 더 오래 눌러도 소용없다. 0911 제보: "길게 눌러도 키보드 안 나와".
 *
 * 이 화면만 2초마다 목록을 통째로 다시 짓고 있었다. 두 겹으로 막는다:
 *   1. 값이 안 바뀌었으면 다시 짓지 않는다 (평소엔 아예 안 짓는다)
 *   2. 바뀌었어도 손이 닿아 있으면 미룬다 */
static bool touching(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

static bool same_as_drawn(const hid_host_t *a, int n)
{
    if (!s_built || n != s_n) return false;
    for (int i = 0; i < n; i++) {
        if (memcmp(a[i].addr, s_hosts[i].addr, 6) != 0) return false;
        if (a[i].here != s_hosts[i].here) return false;
        if (strcmp(a[i].name, s_hosts[i].name) != 0) return false;
    }
    return true;
}

static void relist_cb(lv_timer_t *t)
{
    (void)t;
    hid_host_t now[HID_HOSTS_MAX];
    int n = port_hid_hosts(now, HID_HOSTS_MAX);
    if (same_as_drawn(now, n)) return;
    if (touching()) return;            /* 다음 차례에 다시 본다 */
    show_list();
}

static void addr_txt(const uint8_t a[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* 이름을 다 치면 */
static void name_done(const char *text)
{
    if (text && s_editing >= 0 && s_editing < s_n)
        port_hid_host_name_set(s_hosts[s_editing].addr, text);
    s_editing = -1;
    show_list();
}

/* 짧게 = 그 기기로 옮겨 붙기 */
static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    port_hid_host_pick(s_hosts[i].addr);
    show_list();
}

/* 이름 짓기 — 길게 누르기로도, 줄 오른쪽 연필 단추로도.
 * 🚨 길게 누르기만 두면 아는 사람만 쓴다. 보이는 문이 하나 있어야 한다
 * (뒤로가기 단추를 넣을 때와 같은 이유). */
static void rename_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    s_editing = i;
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; s_built = false; }
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    keypad_open("name this host", s_hosts[i].name, name_done);
}

static void any_cb(lv_event_t *e)
{
    (void)e;
    port_hid_host_any();
    show_list();
}

static void show_list(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_list) { lv_obj_delete(s_list); s_list = NULL; }
    s_n = port_hid_hosts(s_hosts, HID_HOSTS_MAX);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 360, 300);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_n; i++) {
        char sub[24];
        addr_txt(s_hosts[i].addr, sub, sizeof sub);
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 62);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(s_hosts[i].here ? 0x24343A : 0x1D1D24), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_left(b, 18, 0);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, rename_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        /* 연필 — 줄 안에 겹쳐 둔다. 여기를 누르면 줄이 아니라 이게 받는다. */
        lv_obj_t *ed = lv_button_create(b);
        lv_obj_set_size(ed, 54, 46);
        lv_obj_set_style_radius(ed, 14, 0);
        lv_obj_set_style_bg_color(ed, lv_color_hex(0x2A2A34), 0);
        lv_obj_set_style_shadow_width(ed, 0, 0);
        lv_obj_align(ed, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(ed, rename_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *el = lv_label_create(ed);
        lv_label_set_text(el, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(el, lv_color_hex(0xB6C2D6), 0);
        lv_obj_center(el);

        lv_obj_t *t = lv_label_create(b);
        /* 🚨 이름이 없으면 주소를 보인다. 빈 줄을 보이면 고를 수가 없다. */
        lv_label_set_text(t, s_hosts[i].name[0] ? s_hosts[i].name : sub);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -11);

        lv_obj_t *s = lv_label_create(b);
        lv_label_set_text(s, s_hosts[i].here ? "connected" : sub);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(s_hosts[i].here ? 0x5BD48A : 0x8A93A6), 0);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 12);
    }

    if (s_n == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_label_set_text(e, "no paired host yet");
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(0x6E7686), 0);
    } else {
        /* 골라둔 것을 풀어 아무나 받게 — 새 기기에 짝지을 때 필요하다. */
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, 340, 54);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x141418), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, any_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, "accept any");
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0x9AA4AE), 0);
        lv_obj_center(t);
    }

    s_built = true;

    /* 붙는 데 몇 초 걸린다. 상태가 바뀌면 목록이 따라가야 한다.
     * 🚨 lv_timer_cb_t 는 인자를 받는다. show_list 를 그대로 캐스팅해 넘기면
     * 형이 안 맞는 호출이라 플랫폼에 따라 터진다 — 감싸서 넘긴다.
     * 🚨 이 콜백은 **바뀌었을 때만** 다시 짓는다(relist_cb 의 설명). */
    s_poll = lv_timer_create(relist_cb, 2000, NULL);
}

static void host_pick_close(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_scr)  { lv_obj_delete(s_scr); s_scr = NULL; }
    s_list = NULL;
    s_built = false;
}

void host_pick_open(void)
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
    lv_label_set_text(t, "Hosts");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A93A6), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -186);

    lv_obj_t *h = lv_label_create(s_scr);
    lv_label_set_text(h, "tap to switch  -  " LV_SYMBOL_EDIT " to name");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x5A5A66), 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 186);

    ui_back_btn(s_scr, host_pick_close);
    launcher_handle_add(s_scr, host_pick_close);
    show_list();
}
