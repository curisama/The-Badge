/* 디스플레이를 우리가 직접 올린다.
 *
 * 왜 BSP 의 bsp_display_start_with_config() 를 안 쓰나:
 * 그 함수가 패널 핸들을 자기 static 에 숨기고 밖으로 안 내준다. 핸들이 없으면
 * esp_lcd_panel_disp_on_off() 를 못 부르고, 그러면 화면을 "진짜로" 못 끈다.
 * BSP 의 backlight_off 는 밝기를 0 으로 낮출 뿐이라 패널은 계속 스캔한다.
 * 실측(0906): 화면 끔 76mV/h, 화면 켬 171mV/h — 껐는데도 켠 것의 44% 가 흐른다.
 *
 * BSP 를 통째로 들여올(vendoring) 필요는 없다. bsp_display_new() 가 공개
 * 함수고 핸들을 돌려준다. BSP 내부 bsp_display_lcd_init() 이 하던 일이
 * 그것 + 어댑터 등록이 전부라, 그 60줄을 여기로 옮겼다.
 *
 * 대신 BSP 의 static 에 기대던 것들이 빈다 — brightness/rotation/get_input_dev.
 * 셋 다 여기서 우리 핸들로 다시 만든다(0x51, 0x36). 명령 형식은 BSP 것을
 * 그대로 따랐고 MADCTL 은 이미 실기에서 확인됐다.
 *
 * bsp_display_lock/unlock 은 esp_lv_adapter_lock 을 감싼 것뿐이라 그대로 쓴다. */
#include "display.h"
#include "port.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "bsp/display.h"
#include "esp_lv_adapter.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "disp";

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t    s_touch;
static lv_indev_t               *s_indev;
static bool                      s_on = true;
static int                       s_bright = 45;

/* 패널에 명령 한 줄 보내기. BSP 가 쓰던 인코딩 그대로다 —
 * 상위 바이트에 명령, 0x02 는 이 패널의 QSPI 명령 접두사. */
static esp_err_t panel_cmd(uint8_t cmd, const uint8_t *param, size_t len)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    uint32_t lcd_cmd = cmd;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    /* 🚨 그림을 보내는 것과 같은 SPI 버스다. 다른 태스크에서 명령을 끼워넣으면
     * "polling transaction in progress" 로 그리기가 실패한다(0908 검증에서
     * 밝기 바꾸기가 그리기와 부딪혔다 — 여백은 락을 잡았는데 밝기는 안 잡았다).
     * 부르는 쪽마다 챙기게 두면 언젠가 또 빠뜨린다. 여기 한 곳에서 잡는다.
     * LVGL 락은 재귀라서, 이미 쥔 채로 들어와도 안전하다. */
    port_lock();
    esp_err_t r = esp_lcd_panel_io_tx_param(s_io, lcd_cmd, param, len);
    port_unlock();
    return r;
}

/* LVGL 이 넘기는 갱신 영역을 짝수 경계로 맞춘다. 이 패널이 2픽셀 단위로만
 * 받는다 — BSP 의 rounder_event_cb 와 같은 일이다. 안 맞추면 화면이 어긋난다. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 = (a->x1 >> 1) << 1;
    a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1;
    a->y2 = ((a->y2 >> 1) << 1) + 1;
}

bool badge_display_start(void)
{
    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    acfg.task_stack_size = 20 * 1024;
    /* 🚨 PSRAM 에 되돌렸다 (0907 밤).
     * 라이트슬립 때문에 내부 RAM 으로 옮겼는데, 20KB 가 내부 힙 한가운데
     * 자리잡으면서 "연속으로 잡을 수 있는 최대 덩어리"가 128KB → 56KB 로
     * 쪼그라들었다. SPI 가 화면을 보내려면 연속 버퍼가 필요해서, 게임·앱·
     * 마우스(BLE 47KB)까지 겹치면 못 잡고 그리기가 멈춘다
     * ("Failed to allocate priv TX buffer" → "Draw bitmap failed: NO_MEM").
     *
     * 라이트슬립의 진짜 위험은 SPIRAM 반쯤재우기였고 그건 sdkconfig 에서
     * 껐다. 스택 위치는 그것만 꺼두면 안전하다 — 둘 다 막을 필요가 없었다. */
    acfg.stack_in_psram  = true;
    if (esp_lv_adapter_init(&acfg) != ESP_OK) { ESP_LOGE(TAG, "어댑터 init 실패"); return false; }

    const bsp_display_config_t dcfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&dcfg, &s_panel, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "패널 생성 실패");
        return false;
    }
    ESP_LOGI(TAG, "패널 핸들 확보 — 이제 진짜로 끌 수 있다");

    /* 패널은 켜졌지만 GRAM 은 아직 쓰레기다. 밝기를 0 으로 눌러두고
     * 첫 프레임을 그린 뒤에 올린다 — 부팅 때 번쩍하던 것의 정체가 이것이다. */
    badge_display_brightness(0);

    /* 스트랩을 끼우면 위아래가 뒤집힌다. 기준 방향을 180도 돌린다.
     * 🚨 터치는 건드리지 않는다 — 실기에서 이 조합이 맞다(0906 확인). */
    badge_display_rotate180();

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation  = ESP_LV_ADAPTER_ROTATE_0,   /* 이 보드에선 무시된다. 회전은 MADCTL 로 */
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            /* 🚨 한 번에 보내는 줄 수. 이게 곧 "SPI 가 요구하는 연속 내부 메모리"다.
             * 그림 버퍼는 PSRAM 에 있는데 SPI 가 거기서 바로 DMA 를 못 해서,
             * 보낼 때마다 같은 크기의 내부 버퍼를 임시로 잡는다.
             * 50줄이면 466x50x2 = 45.5KB — BLE(47KB)가 올라오면 그 덩어리를
             * 못 잡아 그리기가 통째로 멈춘다(0907 밤: 게임·앱·마우스 뒤
             * "Failed to allocate priv TX buffer" 13,943회).
             * 16줄이면 14.6KB 라 BLE 와 같이 있어도 넉넉하다. 나눠 보내는
             * 횟수가 늘지만 전송량 자체는 같다. */
            /* 🚨 16줄이면 466줄 화면이 30조각이 된다. 조각마다 영역 계산·
             * 작업 등록·SPI 트랜잭션이 붙으므로 조각 수가 곧 값이다.
             * 24줄로 올려 20조각으로 줄인다(0909). 내부 RAM 29.8 → 44.7KB.
             * 물 앱 돌 때 내부 힙 최저가 23 → 61KB 로 늘어 여유가 생겼다.
             * 🚨 use_psram=false 라 DMA 가 이 버퍼에서 바로 나간다 — 아래
             * 주석의 바운스 버퍼 함정과는 무관하다. */
            .buffer_height = 24,
            /* 🚨 PSRAM 에 그리면 SPI 가 거기서 바로 DMA 를 못 해서, 보낼 때마다
             * 같은 크기의 내부 버퍼를 임시로 잡는다. 그 임시 버퍼는 SPI 큐에
             * 쌓인 전송마다 하나씩 필요해서, 빠르게 그리면 곱해진다 —
             * 그래서 조각을 줄여도(45.5KB→14.6KB) 실패가 안 없어졌다.
             *
             * 내부 RAM 에 그리면 DMA 가 직접 읽어 임시 버퍼가 아예 없다.
             * 16줄 x 2장 = 29KB 를 상시로 쓰지만, 실패할 수 있는 런타임 할당이
             * 사라진다. 최대치를 미리 내주는 쪽이 안전하다. */
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) { ESP_LOGE(TAG, "디스플레이 등록 실패"); return false; }
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    bsp_display_cfg_t tcfg = {
        .touch_flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    if (bsp_touch_new(&tcfg, &s_touch) != ESP_OK) { ESP_LOGE(TAG, "터치 생성 실패"); return false; }
    esp_lv_adapter_touch_config_t tch = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch);
    s_indev = esp_lv_adapter_register_touch(&tch);
    if (!s_indev) { ESP_LOGE(TAG, "터치 등록 실패"); return false; }

    if (esp_lv_adapter_start() != ESP_OK) { ESP_LOGE(TAG, "어댑터 start 실패"); return false; }
    ESP_LOGI(TAG, "디스플레이 준비 완료 (밝기는 아직 0)");
    return true;
}

lv_indev_t *badge_display_indev(void) { return s_indev; }

/* 🚨 MADCTL 을 날것으로 쓰면 안 된다.
 * 예전엔 0x36 에 0xC0 을 그대로 밀어넣었는데, CO5300 드라이버는 madctl_val 을
 * 자기가 쥐고 초기화 때 색 순서(RGB/BGR) 같은 비트를 세워둔다. 통째로 덮으면
 * 그 비트가 날아가 화면 가장자리에 색 띠가 남는다(0906: 오른쪽 초록 줄).
 * 드라이버의 mirror 를 쓰면 기존 값 위에 비트 6·7 만 얹는다. */
/* 180도 뒤집으면 x 오프셋이 반대편에서 계산된다.
 * 이 패널은 초기화가 열을 6..471 로 잡는다(왼쪽 여백 6). 뒤집으면 컨트롤러가
 * 열 주소를 반대쪽부터 세므로, 맞는 여백은 (컨트롤러 열 수 - 1 - 471) 이다.
 *   열 480 → 8 · 478 → 6(그대로) · 476 → 4 · 472 → 0
 * 컨트롤러 열 수를 문서 없이 알 수 없어서 설정에서 고를 수 있게 했다.
 * 화면 가장자리에 색 띠가 보이면 그게 안 맞는 것이다. */
/* 기본 8. CO5300 데이터시트가 내부 GRAM 을 480x480 이라 한다:
 *     6(왼쪽 여백) + 466(패널) + 8 = 480
 * 뒤집으면 맞는 여백은 480-1-471 = 8 이다. 그래도 안 맞으면 설정에서 돌린다. */
static int s_xgap = 8;

void badge_display_set_xgap(int gap)
{
    if (gap < 0) gap = 0;
    if (gap > 16) gap = 16;
    /* 값이 그대로면 아무것도 안 한다. 전체 다시 그리기는 비싼 일이고,
     * 짧은 사이에 여러 번 몰리면 SPI DMA 버퍼가 말라 그리기가 실패한다
     * (0907 공격 검증: 1.2초에 40번 바꾸니 ESP_ERR_NO_MEM). 부팅 때
     * settings_load 가 저장값을 다시 세우는 흔한 경우도 여기서 걸러진다. */
    if (gap == s_xgap && s_panel) return;
    s_xgap = gap;
    if (s_panel) esp_lcd_panel_set_gap(s_panel, s_xgap, 0);
    ESP_LOGI(TAG, "x 여백 %d", s_xgap);
    /* 🚨 창이 바뀌었으니 다시 그려야 하는데, LVGL 을 만지려면 락이 필요하다.
     * 예전엔 그냥 불렀다 — 설정 화면에서는 LVGL 태스크 안이라 우연히
     * 괜찮았지만, 부팅 때 settings_load() 가 부르는 경로는 LVGL 태스크가
     * 이미 도는 중이라 경합이다(0907 공격 검증에서 lv_inv_area 안에 갇혔다).
     * 어댑터 락은 재귀 뮤텍스라 LVGL 태스크 안에서 또 잡아도 안전하다. */
    port_lock();
    if (lv_screen_active()) lv_obj_invalidate(lv_screen_active());
    port_unlock();
}

int badge_display_get_xgap(void) { return s_xgap; }

void badge_display_rotate180(void)
{
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_set_gap(s_panel, s_xgap, 0);
    ESP_LOGI(TAG, "기준 방향 180도 (mirror x+y, x여백 %d)", s_xgap);
}

void badge_display_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent > 0) s_bright = percent;      /* 0 은 "끔"이라 기억하지 않는다 */
    uint8_t v = (uint8_t)(percent * 255 / 100);
    panel_cmd(0x51, &v, 1);
}

int badge_display_brightness_get(void) { return s_bright; }

/* 얼마나 깊이 끄나.
 *   0x28 Display Off  — 스캔을 멈춘다. 드라이버 로직과 부스트는 계속 돈다
 *   0x10 Sleep In     — 부스트·오실레이터까지 내린다. 제일 적게 먹는다
 * AMOLED 는 발광 전원(ELVDD/ELVSS)을 드라이버 옆 DC-DC 가 만드는데, 그게
 * 밝기와 무관하게 돌기 때문에 0x28 만으로는 덜 준다. 그래서 0x10 까지 간다.
 * 대신 깨울 때 0x11 뒤에 안정화 시간이 필요하고, 설정이 초기화될 수 있어
 * 방향(MADCTL)과 밝기를 다시 세운다. */
#define LCD_CMD_SLPIN   0x10
#define LCD_CMD_SLPOUT  0x11

void badge_display_on(bool on)
{
    if (on == s_on) return;
    s_on = on;

    if (on) {
        panel_cmd(LCD_CMD_SLPOUT, NULL, 0);
        /* 데이터시트가 요구하는 안정화 시간. 이걸 안 주면 뒤따르는 명령이
         * 먹지 않아 화면이 깨진 채로 올라온다. */
        vTaskDelay(pdMS_TO_TICKS(120));
        badge_display_rotate180();               /* 잠들며 날아갔을 수 있다 */
        esp_lcd_panel_disp_on_off(s_panel, true);
        badge_display_brightness(s_bright);
        /* GRAM 이 보존됐는지 장담할 수 없으니 한 판 다시 그린다.
         * 여기도 락을 잡는다 — 재귀 뮤텍스라 이미 잡고 있어도 안전하다. */
        port_lock();
        if (lv_screen_active()) lv_obj_invalidate(lv_screen_active());
        port_unlock();
    } else {
        badge_display_brightness(0);
        esp_lcd_panel_disp_on_off(s_panel, false);   /* 0x28 */
        panel_cmd(LCD_CMD_SLPIN, NULL, 0);           /* 0x10 — 부스트까지 */
    }
    ESP_LOGI(TAG, "패널 %s", on ? "ON (0x11+0x29)" : "OFF (0x28+0x10)");
}
