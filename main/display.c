/* The display is brought up here rather than by the BSP.
 *
 * Why not bsp_display_start_with_config(): it keeps the panel handle in its
 * own static and never hands it out. Without the handle there is no way to
 * call esp_lcd_panel_disp_on_off(), and therefore no way to turn the display
 * *actually* off. The BSP's backlight_off only drops the brightness to zero
 * and the panel keeps scanning — measured, "off" still drew 76 mV/h against
 * 171 on, or 44% of being on.
 *
 * Vendoring the whole BSP is not necessary. bsp_display_new() is public and
 * returns the handle. All the BSP's internal bsp_display_lcd_init() did was
 * that plus registering the adapter, so those sixty lines moved here.
 *
 * What that leaves missing are the things that relied on the BSP's statics —
 * brightness, rotation and get_input_dev. All three are rebuilt here against
 * our own handle (0x51, 0x36). The command encoding follows the BSP's, and
 * the MADCTL value was already confirmed on the board.
 *
 * bsp_display_lock/unlock are only wrappers around esp_lv_adapter_lock and
 * are used as they are. */
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

/* Send one command to the panel, in the encoding the BSP used — the command
 * in the high byte, with 0x02 as this panel's QSPI command prefix. */
static esp_err_t panel_cmd(uint8_t cmd, const uint8_t *param, size_t len)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    uint32_t lcd_cmd = cmd;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    /* 🚨 This is the same SPI bus the pixels go out on. A command slipped in
     * from another task makes drawing fail with "polling transaction in
     * progress" (changing the brightness collided with drawing: the margin
     * code took the lock and the brightness code did not).
     * Leaving it to each caller means someone forgets eventually, so it is
     * taken here, in one place. The LVGL lock is recursive, so entering while
     * already holding it is safe. */
    port_lock();
    esp_err_t r = esp_lcd_panel_io_tx_param(s_io, lcd_cmd, param, len);
    port_unlock();
    return r;
}

/* Round LVGL's invalidated area to even boundaries — this panel only accepts
 * two-pixel units (the same job as the BSP's rounder_event_cb). Without it the
 * image is offset. */
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
    /* 🚨 Moved back to PSRAM.
     * It had been moved into internal RAM because of light sleep, but 20 KB
     * landing in the middle of the internal heap shrank the largest
     * contiguous block from 128 KB to 56 KB. SPI needs a contiguous buffer to
     * push a frame, so with a game, an app and the mouse (BLE, 47 KB) all
     * present it could not get one and drawing stopped.
     *
     * The real danger with light sleep was half-sleeping SPIRAM, and that is
     * disabled in sdkconfig. With that off, this stack placement is safe —
     * both defences were never needed. */
    acfg.stack_in_psram  = true;
    if (esp_lv_adapter_init(&acfg) != ESP_OK) { ESP_LOGE(TAG, "adapter init failed"); return false; }

    const bsp_display_config_t dcfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&dcfg, &s_panel, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel creation failed");
        return false;
    }
    ESP_LOGI(TAG, "panel handle acquired — it can now really be switched off");

    /* The panel is on but GRAM still holds garbage. Hold the brightness at
     * zero and raise it after the first frame — that flash at boot was this. */
    badge_display_brightness(0);

    /* Worn on a strap it is upside down, so the reference orientation is
     * rotated 180. 🚨 Touch is left alone — this combination is what matches
     * the hardware. */
    badge_display_rotate180();

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation  = ESP_LV_ADAPTER_ROTATE_0,   /* ignored on this board; rotation is done with MADCTL */
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            /* 🚨 Lines per transfer. This is exactly "how much contiguous
             * internal memory SPI demands".
             * The draw buffer is in PSRAM and SPI cannot DMA from there, so
             * every transfer temporarily allocates an internal buffer of the
             * same size. At 50 lines that is 466x50x2 = 45.5 KB — and once
             * BLE (47 KB) is up, that block cannot be found and drawing stops
             * entirely ("Failed to allocate priv TX buffer", 13,943 times
             * after a game, an app and the mouse).
             * At 16 lines it is 14.6 KB, which is comfortable alongside BLE.
             * There are more transfers, but the same number of bytes. */
            /* 🚨 At 16 lines a 466-line screen is thirty pieces, and each
             * piece costs an area calculation, a work item and an SPI
             * transaction — the piece count *is* the cost. Raised to 24 lines
             * for twenty pieces: internal RAM 29.8 -> 44.7 KB, and the
             * internal heap low-water mark while the water app runs went from
             * 23 to 61 KB, which is real headroom.
             * 🚨 With use_psram=false, DMA reads straight from this buffer —
             * unrelated to the bounce-buffer trap described below. */
            .buffer_height = 24,
            /* 🚨 Drawing into PSRAM means SPI cannot DMA from it, so every
             * transfer temporarily allocates an internal buffer of the same
             * size. One is needed per transfer queued in SPI, so drawing fast
             * multiplies them — which is why shrinking the piece
             * (45.5 KB -> 14.6 KB) did not make the failures go away.
             *
             * Drawing into internal RAM lets DMA read it directly and there is
             * no temporary buffer at all. It costs 16 lines x 2 buffers =
             * 29 KB permanently, but removes a runtime allocation that can
             * fail. Paying the maximum up front is the safer trade. */
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) { ESP_LOGE(TAG, "display registration failed"); return false; }
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    bsp_display_cfg_t tcfg = {
        .touch_flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    if (bsp_touch_new(&tcfg, &s_touch) != ESP_OK) { ESP_LOGE(TAG, "touch creation failed"); return false; }
    esp_lv_adapter_touch_config_t tch = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch);
    s_indev = esp_lv_adapter_register_touch(&tch);
    if (!s_indev) { ESP_LOGE(TAG, "touch registration failed"); return false; }

    if (esp_lv_adapter_start() != ESP_OK) { ESP_LOGE(TAG, "adapter start failed"); return false; }
    ESP_LOGI(TAG, "display ready (brightness still 0)");
    return true;
}

lv_indev_t *badge_display_indev(void) { return s_indev; }

/* 🚨 Never write MADCTL raw.
 * 0xC0 used to be pushed straight into 0x36, but the CO5300 driver owns
 * madctl_val and sets bits in it during init, such as the colour order
 * (RGB/BGR). Overwriting the whole byte loses those and leaves a coloured
 * band down the edge of the screen (a green stripe on the right).
 * The driver's mirror call adds only bits 6 and 7 on top of what is there. */
/* Flipping 180 degrees makes the x offset count from the other side.
 * This panel's init sets columns 6..471 (a 6-pixel left margin). Flipped, the
 * controller counts column addresses from the opposite end, so the correct
 * margin is (controller columns - 1 - 471).
 *   480 columns -> 8, 478 -> 6 (unchanged), 476 -> 4, 472 -> 0
 * The controller's column count cannot be known without documentation, so it
 * is selectable in Settings. A coloured band at the edge of the screen means
 * it is wrong. */
/* Default 8. The CO5300 datasheet gives internal GRAM as 480x480:
 *     6 (left margin) + 466 (panel) + 8 = 480
 * Flipped, the correct margin is 480-1-471 = 8. If it still looks wrong,
 * change it in Settings. */
static int s_xgap = 8;

void badge_display_set_xgap(int gap)
{
    if (gap < 0) gap = 0;
    if (gap > 16) gap = 16;
    /* Do nothing if the value is unchanged. A full redraw is expensive, and
     * several in quick succession drain the SPI DMA buffers and make drawing
     * fail (forty changes in 1.2 seconds gave ESP_ERR_NO_MEM). It also
     * filters the common case of settings_load re-applying the stored value
     * at boot. */
    if (gap == s_xgap && s_panel) return;
    s_xgap = gap;
    if (s_panel) esp_lcd_panel_set_gap(s_panel, s_xgap, 0);
    ESP_LOGI(TAG, "x margin %d", s_xgap);
    /* 🚨 The window changed, so it has to be redrawn — and touching LVGL
     * requires the lock. It used to be called without one: from the Settings
     * screen that happened to be inside the LVGL task and was fine, but the
     * path where settings_load() calls it at boot races the LVGL task that is
     * already running (it got stuck inside lv_inv_area during a stress test).
     * The adapter lock is a recursive mutex, so taking it again from inside
     * the LVGL task is safe. */
    port_lock();
    if (lv_screen_active()) lv_obj_invalidate(lv_screen_active());
    port_unlock();
}

int badge_display_get_xgap(void) { return s_xgap; }

void badge_display_rotate180(void)
{
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_set_gap(s_panel, s_xgap, 0);
    ESP_LOGI(TAG, "reference orientation 180 (mirror x+y, x margin %d)", s_xgap);
}

void badge_display_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent > 0) s_bright = percent;      /* 0 means "off" and is not remembered */
    uint8_t v = (uint8_t)(percent * 255 / 100);
    panel_cmd(0x51, &v, 1);
}

int badge_display_brightness_get(void) { return s_bright; }

/* How deeply to switch it off.
 *   0x28 Display Off  — stops the scan; the driver logic and the boost keep running
 *   0x10 Sleep In     — takes the boost and the oscillator down too; the least power
 * An AMOLED's emission supply (ELVDD/ELVSS) comes from a DC-DC next to the
 * driver and runs regardless of brightness, so 0x28 alone does not save much.
 * Hence going as far as 0x10.
 * The cost is that waking needs a settling delay after 0x11, and settings may
 * be reset — so the orientation (MADCTL) and the brightness are reapplied. */
#define LCD_CMD_SLPIN   0x10
#define LCD_CMD_SLPOUT  0x11

void badge_display_on(bool on)
{
    if (on == s_on) return;
    s_on = on;

    if (on) {
        panel_cmd(LCD_CMD_SLPOUT, NULL, 0);
        /* The settling time the datasheet requires. Without it the commands
         * that follow do not take and the display comes up corrupted. */
        vTaskDelay(pdMS_TO_TICKS(120));
        badge_display_rotate180();               /* may have been lost while asleep */
        esp_lcd_panel_disp_on_off(s_panel, true);
        badge_display_brightness(s_bright);
        /* There is no guarantee GRAM survived, so redraw a full frame.
         * This takes the lock too — recursive, so already holding it is safe. */
        port_lock();
        if (lv_screen_active()) lv_obj_invalidate(lv_screen_active());
        port_unlock();
    } else {
        badge_display_brightness(0);
        esp_lcd_panel_disp_on_off(s_panel, false);   /* 0x28 */
        panel_cmd(LCD_CMD_SLPIN, NULL, 0);           /* 0x10 — down to the boost */
    }
    ESP_LOGI(TAG, "panel %s", on ? "ON (0x11+0x29)" : "OFF (0x28+0x10)");
}
