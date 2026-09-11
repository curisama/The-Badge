/* 발표 리모컨. 슬라이드 넘기고, 잠깐 화면을 검게 만든다.
 * 단추가 커야 한다 — 발표 중엔 화면을 안 보고 누른다. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"

#define K_PGUP  0x4B
#define K_PGDN  0x4E
#define K_B     0x05
#define K_ESC   0x29
#define K_F5    0x3E

static lv_obj_t *s_state;

typedef struct { const char *label; unsigned mod, code; const char *say; } pkey_t;


static void hit(lv_event_t *e)
{
    const pkey_t *k = (const pkey_t *)lv_event_get_user_data(e);
    port_hid_key(k->mod, k->code);
    lv_label_set_text(s_state, k->say);
}

static lv_obj_t *btn(lv_obj_t *root, const char *txt, int w, int h, int dx, int dy,
                     uint32_t bg, const pkey_t *k, const lv_font_t *font)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_event_cb(b, hit, LV_EVENT_CLICKED, (void *)k);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    return b;
}

static const pkey_t NEXT  = { "", 0, K_PGDN, "next" };
static const pkey_t PREV  = { "", 0, K_PGUP, "prev" };
static const pkey_t BLACK = { "", 0, K_B,    "black" };
static const pkey_t START = { "", 0, K_F5,   "start" };
static const pkey_t QUIT  = { "", 0, K_ESC,  "esc" };

void present_build(lv_obj_t *root)
{
    /* 다음 슬라이드가 제일 많이 눌린다. 화면 절반을 준다. */
    btn(root, LV_SYMBOL_RIGHT, 210, 150, 0, -92, 0x2E6E9E, &NEXT,  &lv_font_montserrat_48);
    btn(root, LV_SYMBOL_LEFT,  210,  90, 0,  22, 0x24242A, &PREV,  &lv_font_montserrat_32);
    btn(root, "B",              96,  72, -78, 118, 0x24242A, &BLACK, &lv_font_montserrat_26);
    btn(root, "F5",             96,  72,  78, 118, 0x24242A, &START, &lv_font_montserrat_26);
    /* 아래쪽은 홈 손잡이 자리다. Esc 는 위로 올린다. */
    btn(root, "Esc",           110,  52,   0, -176, 0x1C1C22, &QUIT,  &lv_font_montserrat_20);

    s_state = lv_label_create(root);
    lv_label_set_text(s_state, "");
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x5BD48A), 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, 176);
}

void present_free(void) {}
