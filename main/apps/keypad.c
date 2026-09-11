/* 멀티탭 키패드 — 옛날 핸드폰처럼 한 키를 여러 번 눌러 글자를 고른다.
 *
 * 🚨 왜 쿼티가 아닌가: 466px 원형에 26키를 넣으면 키 하나가 50×40px 이고,
 * 둥근 모서리가 양 끝 키를 먹는다. 12키면 104×68px — 세 배 넓다.
 *
 * 🚨 그런데 "원이니까 아무 데나 놓으면 된다" 가 아니다. 3칸 가로줄(약 340px)이
 * 들어가려면 중심에서 세로로 159px 안쪽이어야 한다:
 *     반지름 233, 가로 반폭 170 필요 → √(233² - 170²) = 159
 * 그 바깥은 폭이 모자라 칸이 잘린다. 그래서 네 줄을 dy -118 ~ +98 안에 욱여
 * 넣었고, 칸 높이가 68px 이 됐다. 제일 먼 모서리가 중심에서 222px 이다.
 *
 * 🚨 다음칸(→) 키가 반드시 있어야 한다. 시간 초과만 두면 "ab" 를 못 친다 —
 * 같은 키에 든 두 글자를 잇달아 칠 방법이 없기 때문이다. WiFi 비밀번호에
 * 그런 조합은 흔하다.
 *
 * 🚨 친 글자를 가리지 않는다. 멀티탭으로 스무 자를 눈 감고 치는 건 불가능하다.
 * 손에 들고 보는 기기라 어깨너머 위험도 낮다.
 */
#include "app.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

#define KP_MAX 64                /* WPA2 비밀번호 최대 63자 + 끝 */
#define KP_TAP_MS 800            /* 같은 키를 이 안에 다시 누르면 다음 글자 */

/* 키마다 든 글자. 모드 넷 × 아홉 키 + 0번 키.
 * 🚨 비밀번호용이라 기호를 넉넉히 뒀다 — 영문만 되면 반쯤 쓸모없다. */
static const char *SET[4][10] = {
    /* abc */ { " 0", ".,?!-", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" },
    /* ABC */ { " 0", ".,?!-", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" },
    /* 123 */ { "0",  "1", "2", "3", "4", "5", "6", "7", "8", "9" },
    /* sym */ { " 0", "@#$%", "&*()", "-_=+", "[]{}", "<>/\\", ":;'\"", "!?.,", "^~`|", "" },
};
static const char *MODE_NAME[4] = { "abc", "ABC", "123", "!@#" };

static char       s_buf[KP_MAX];
static int        s_len;
static uint8_t    s_mode;
static int        s_last_key = -1;      /* 방금 누른 키 (-1 = 없음) */
static int        s_tap;                /* 그 키를 몇 번째 눌렀나 */
static uint32_t   s_tap_ms;
static lv_obj_t  *s_scr, *s_field, *s_modelbl;
static lv_obj_t  *s_keylbl[10];        /* 키 얼굴 — 모드마다 다시 칠한다 */
static void      (*s_done)(const char *text);

/* 🚨 키 얼굴이 고정이면 모드를 바꿔도 뭐가 들었는지 안 보인다 — 기호 모드로
 * 가도 "2 abc" 라고 적혀 있으면 아무도 기호를 못 찾는다(0911 지적).
 * 모드가 바뀔 때마다 아홉 키를 다시 칠한다. */
static void kp_face(int k, char *out, size_t cap)
{
    const char *set = SET[s_mode][k];
    if (s_mode == 2) {                      /* 숫자 — 숫자 하나만 크게 */
        snprintf(out, cap, "%s", set);
    } else if (s_mode == 3) {               /* 기호 — 든 것을 그대로 */
        snprintf(out, cap, "%s", set[0] ? set : "-");
    } else if (k == 0) {
        snprintf(out, cap, "0 _");          /* 0 과 빈칸 */
    } else {
        snprintf(out, cap, "%d %s", k, set); /* 숫자 + 글자 */
    }
}

static void kp_paint(void)
{
    if (!s_field) return;
    /* 오른쪽 정렬 — 길어지면 앞쪽이 아니라 방금 친 쪽이 보여야 한다. */
    lv_label_set_text(s_field, s_len ? s_buf : " ");
    if (s_modelbl) lv_label_set_text(s_modelbl, MODE_NAME[s_mode]);
    for (int k = 0; k < 10; k++) {
        if (!s_keylbl[k]) continue;
        char f[16];
        kp_face(k, f, sizeof f);
        lv_label_set_text(s_keylbl[k], f);
    }
}

/* 글자를 확정한다 — 다음에 다른 키를 누르거나 →/시간 초과일 때. */
static void kp_commit(void)
{
    s_last_key = -1;
    s_tap = 0;
}

static void kp_key(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    const char *set = SET[s_mode][k];
    if (!set || !set[0]) return;
    int n = (int)strlen(set);
    uint32_t now = lv_tick_get();

    if (k == s_last_key && now - s_tap_ms < KP_TAP_MS && s_len > 0) {
        /* 같은 키를 이어서 — 마지막 글자를 다음 것으로 바꾼다 */
        s_tap = (s_tap + 1) % n;
        s_buf[s_len - 1] = set[s_tap];
    } else {
        if (s_len >= KP_MAX - 1) return;
        s_tap = 0;
        s_buf[s_len++] = set[0];
        s_buf[s_len] = '\0';
        s_last_key = k;
    }
    s_tap_ms = now;
    kp_paint();
}

static void kp_next(lv_event_t *e)   { (void)e; kp_commit(); }

static void kp_mode(lv_event_t *e)
{
    (void)e;
    s_mode = (uint8_t)((s_mode + 1) % 4);
    kp_commit();
    kp_paint();
}

static void kp_back(lv_event_t *e)
{
    (void)e;
    if (s_len > 0) s_buf[--s_len] = '\0';
    kp_commit();
    kp_paint();
}

static void kp_ok(lv_event_t *e)
{
    (void)e;
    void (*cb)(const char *) = s_done;
    s_done = NULL;
    /* 🚨 부르기 전에 화면을 걷는다. 부르는 쪽이 다른 화면을 띄우는데 이게
     * 위에 남아 있으면 그 화면을 덮는다. */
    keypad_close();
    if (cb) cb(s_buf);
}

static void kp_cancel(void);
static void kp_cancel_btn(lv_event_t *e) { (void)e; kp_cancel(); }

/* 손잡이를 올려도, 뒤로 단추를 눌러도 취소다. */
static void kp_cancel(void)
{
    void (*cb)(const char *) = s_done;
    s_done = NULL;
    keypad_close();
    if (cb) cb(NULL);
}

static lv_obj_t *kp_btn(lv_obj_t *root, int dx, int dy, int w, int h,
                        const char *txt, lv_event_cb_t cb, int arg, uint32_t col)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x44444E), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD2D8E4), 0);
    lv_obj_center(l);
    return b;
}

void keypad_open(const char *title, const char *initial,
                 void (*done)(const char *text))
{
    s_done = done;
    s_mode = 0;
    s_last_key = -1;
    s_tap = 0;
    s_len = 0;
    s_buf[0] = '\0';
    if (initial) {
        snprintf(s_buf, sizeof s_buf, "%s", initial);
        s_len = (int)strlen(s_buf);
    }

    /* 🚨 맨 위 층에 띄운다. 부르는 앱의 화면을 안 건드리고 덮었다가, 끝나면
     * 그대로 걷으면 된다 — 앱이 제 화면을 다시 지을 이유가 없다. */
    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   /* 뒤로 안 새게 */

    lv_obj_t *t = lv_label_create(s_scr);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x6E7686), 0);
    /* 🚨 -222 는 원의 폭이 142px 밖에 안 되는 자리라 제목이 잘렸다(0911 시뮬).
     * -206 이면 218px 이라 들어간다. */
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -206);

    /* 친 글자. 🚨 가리지 않는다 — 위 주석 참고. */
    s_field = lv_label_create(s_scr);
    lv_obj_set_width(s_field, 156);
    lv_label_set_long_mode(s_field, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_field, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_field, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_text_align(s_field, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_field, LV_ALIGN_CENTER, 0, -172);

    /* 윗줄: [뒤로] [ 친 글자 ] [OK]. 🚨 되돌아갈 데가 있는 화면이라 손잡이만
     * 두지 않고 단추를 보인다 — 손잡이는 아는 사람만 쓴다(0911 지적).
     * → 는 아랫줄로 내렸다. */
    kp_btn(s_scr, -102, -172, 44, 40, LV_SYMBOL_LEFT, kp_cancel_btn, 0, 0x24242A);
    kp_btn(s_scr,  102, -172, 44, 40, LV_SYMBOL_OK,   kp_ok,        0, 0x2E6E5A);

    /* 숫자 아홉 + 아래 줄 넷. 얼굴은 kp_paint 가 모드에 맞춰 칠한다. */
    for (int i = 0; i < 9; i++) {
        int dx = (i % 3 - 1) * 110;
        int dy = -100 + (i / 3) * 72;
        lv_obj_t *b = kp_btn(s_scr, dx, dy, 104, 68, "", kp_key, i + 1, 0x1D1D24);
        s_keylbl[i + 1] = lv_obj_get_child(b, 0);
    }
    /* 🚨 아랫줄은 넷이다. dy 116 에서 원의 반폭이 178px 이라 88px 칸 넷(352)이
     * 들어간다. → 를 여기 둬야 윗줄에 뒤로가기 자리가 난다. */
    lv_obj_t *mb = kp_btn(s_scr, -132, 116, 88, 68, "abc", kp_mode, 0, 0x24242A);
    s_modelbl = lv_obj_get_child(mb, 0);
    lv_obj_t *zb = kp_btn(s_scr, -44, 116, 88, 68, "", kp_key, 0, 0x1D1D24);
    s_keylbl[0] = lv_obj_get_child(zb, 0);
    kp_btn(s_scr,  44, 116, 88, 68, LV_SYMBOL_RIGHT,     kp_next, 0, 0x24242A);
    kp_btn(s_scr, 132, 116, 88, 68, LV_SYMBOL_BACKSPACE, kp_back, 0, 0x24242A);

    /* 손잡이를 올리면 취소. 앱에서 나가는 것과 같은 손짓이라 따로 안 가르쳐도 된다. */
    launcher_handle_add(s_scr, kp_cancel);
    kp_paint();          /* 🚨 키 얼굴은 여기서 처음 칠해진다 */
}

void keypad_close(void)
{
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_field = s_modelbl = NULL;
    for (int i = 0; i < 10; i++) s_keylbl[i] = NULL;
    s_done = NULL;
}

bool keypad_is_open(void) { return s_scr != NULL; }
