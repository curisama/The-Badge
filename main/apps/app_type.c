/* 문자열 타이핑기. 배지가 키보드인 척하고 대신 쳐준다.
 * 폰 소프트 키보드로 RDP 로그인 창에 긴 비밀번호를 치는 게 고통이라 만들었다.
 *
 * 내용은 main/snippets.h 에 있고 .gitignore 로 빠져 있다. 없으면 예시가 뜬다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"

#if __has_include("snippets.h")
#   include "snippets.h"
#endif

typedef struct { const char *label; const char *text; } snip_t;

static const snip_t SNIPS[] = {
#ifdef BADGE_SNIPPETS
    BADGE_SNIPPETS
#else
    /* 🚨 이 라벨은 Montserrat 로 찍힌다 — 한글을 넣으면 깨진다.
     * (BADGE_SNIPPETS 로 넣는 문구도 마찬가지다. 한글을 쓰려면
     *  tools/make-kr-font.sh 에 그 글자를 넣고 폰트를 바꿔야 한다.) */
    { "no snippets", "" },
#endif
};
#define SNIP_CNT (sizeof(SNIPS) / sizeof(SNIPS[0]))

static lv_obj_t *s_state;

static void tap_cb(lv_event_t *e)
{
    const snip_t *s = (const snip_t *)lv_event_get_user_data(e);
    if (!s->text[0]) return;
    port_hid_type(s->text);
    lv_label_set_text_fmt(s_state, "sent  %s", s->label);
}

void type_build(lv_obj_t *root)
{
    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, "Type");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -160);

    /* 한 줄에 하나씩 큼직하게. 잘못 누르면 엉뚱한 게 타이핑되니 작으면 안 된다. */
    int n = SNIP_CNT > 4 ? 4 : SNIP_CNT;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 280, 62);
        lv_obj_set_style_radius(b, 31, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x24242A), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A6EA5), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_align(b, LV_ALIGN_CENTER, 0, -100 + i * 74);
        lv_obj_add_event_cb(b, tap_cb, LV_EVENT_CLICKED, (void *)&SNIPS[i]);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, SNIPS[i].label);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }

    s_state = lv_label_create(root);
    lv_label_set_text(s_state, "");
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x5BD48A), 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, 168);
}

