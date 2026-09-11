/* USB 저장장치 모드 — 가짜 FAT 을 호스트에 물린다.
 *
 * 여기는 "배선" 만 한다. 판을 짓는 일은 전부 usb_export.c 가 하고, 이 파일은
 * TinyUSB 가 묻는 것에 답한다: 몇 섹터냐, 이 섹터를 달라, 쓸 수 있냐.
 *
 * 🚨 **읽기 전용으로 못 박는다.** 쓰기를 열어두면 윈도우가 붙자마자
 * `System Volume Information` 을 만들려 든다. 우리 판은 그때그때 지어내는
 * 가짜라 쓰기를 받을 데가 없다. "쓰기 금지 매체" 라고 알리면 윈도우가
 * 순순히 받아들이고 아무것도 안 만든다 — 거절하는 것보다 조용하다.
 *
 * 🚨 **나갈 때는 재부팅한다.** TinyUSB 0.15 엔 정리(teardown) 가 없다
 * (`tusb_teardown` 이 헤더에 주석으로만 있다). PHY 를 OTG 에서 뺏어
 * USB-Serial/JTAG 으로 되돌리는 길이 검증된 적이 없고, 반쯤 되돌린 상태로
 * 남으면 COM 이 안 잡혀서 굽지도 못한다. 재부팅은 2초면 끝나고 결과가
 * 확실하다. 들어올 때는 반대다 — PHY 를 가져오는 방향은 esp_tinyusb 가 늘
 * 하는 일이라 그 자리에서 바꾼다.
 *
 * 🚨 벽돌 안 된다. 어떤 상태로 뻗어도 ROM 부트로더는 항상 USB-Serial/JTAG
 * 으로 올라온다 — BOOT 누른 채 RESET 이면 다시 구울 수 있다. */
#include "usb_export.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include <string.h>

static const char *TAG = "usb_msc";

static usb_phy_handle_t s_phy;
static TaskHandle_t     s_task;
static volatile bool    s_active;
static volatile bool    s_ejected;     /* 호스트가 빼갔다 */

/* ── 서술자 ──────────────────────────────────────────────────
 * 🚨 VID 는 에스프레시프 것(0x303A)을 쓴다. 남의 VID 를 빌려 쓰면 그 회사
 * 드라이버가 끼어들 수 있다. PID 는 에스프레시프가 자작용으로 열어 둔
 * 0x4000 대에서 MSC 자리를 쓴다. */
#define USB_VID   0x303A
#define USB_PID   0x4002
#define USB_BCD   0x0200

static const tusb_desc_device_t s_desc_dev = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
#define EPNUM_MSC_OUT   0x01
#define EPNUM_MSC_IN    0x81
#define CFG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

/* 🚨 일련번호는 있어야 한다. 없으면 윈도우가 꽂을 때마다 새 장치로 보고
 * 드라이브 문자를 매번 다르게 준다. */
static const char *const s_desc_str[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: 영어(미국) */
    "Badge",                        /* 1: 만든 이 */
    "Badge Recorder",               /* 2: 제품 */
    "BADGE0001",                    /* 3: 일련번호 */
    "Recordings",                   /* 4: MSC 인터페이스 */
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&s_desc_dev; }

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    uint8_t n;
    if (index == 0) {
        memcpy(&buf[1], s_desc_str[0], 2);
        n = 1;
    } else {
        if (index >= sizeof(s_desc_str) / sizeof(s_desc_str[0])) return NULL;
        const char *s = s_desc_str[index];
        n = (uint8_t)strlen(s);
        if (n > 31) n = 31;
        for (uint8_t i = 0; i < n; i++) buf[1 + i] = s[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

/* ── SCSI ────────────────────────────────────────────────── */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vid[8], uint8_t pid[16], uint8_t rev[4])
{
    (void)lun;
    memcpy(vid, "Badge   ", 8);
    memcpy(pid, "Recordings      ", 16);
    memcpy(rev, "1.0 ", 4);
}

/* 🚨 "매체가 있나" 에 거짓을 돌려주면 호스트는 드라이브가 빈 것으로 본다.
 * 배지에서 "끝" 을 누르면 먼저 이걸로 **빠졌다고 알리고** 잠깐 뒤에 선을
 * 놓는다 — 그래야 윈도우가 놀라지 않는다. */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  /* 매체 없음 */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = usb_export_sectors();
    *block_size  = (uint16_t)usb_export_sector_size();
}

/* 사람이 윈도우에서 "안전하게 제거" 를 누르면 여기로 온다. */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition;
    if (load_eject && !start) {
        ESP_LOGI(TAG, "호스트가 빼갔다 — COM 으로 되돌아간다");
        s_ejected = true;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    const uint32_t sec = usb_export_sector_size();
    if (lba >= usb_export_sectors()) return -1;
    /* 🚨 호스트가 섹터 한가운데부터 달라고 할 수 있다. 섹터를 통째로 지어
     * 놓고 거기서 잘라 준다 — 반쪽만 지으면 경계 계산이 두 벌이 된다. */
    static uint8_t s_sec[512];
    if (sec > sizeof s_sec) return -1;
    if (!usb_export_read(lba, s_sec)) return -1;
    if (offset >= sec) return 0;
    uint32_t n = sec - offset;
    if (n > bufsize) n = bufsize;
    memcpy(buffer, s_sec + offset, n);
    return (int32_t)n;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;                    /* 읽기 전용 매체 */
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;                       /* 여기 올 일이 없다 */
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;                    /* 잠글 것이 없다 */
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/* ── 돌리기 ──────────────────────────────────────────────── */
static void usb_task(void *arg)
{
    (void)arg;
    /* 🚨 tusb_init 을 **이 태스크 안에서** 부른다. 그 안에서 인터럽트를
     * 잡는데(esp_intr_alloc), 인터럽트는 부른 코어에 붙는다. 딴 데서
     * 부르면 인터럽트와 처리 태스크가 다른 코어에 흩어진다. */
    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init 실패");
        s_active = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "드라이브로 올라왔다 — %d개 / %lu섹터",
             usb_export_files(), (unsigned long)usb_export_sectors());
    while (s_active) tud_task();
    vTaskDelete(NULL);
}

bool usb_msc_start(void)
{
    if (s_active) return true;

    usb_export_build();
    s_ejected = false;

    /* PHY 를 USB-Serial/JTAG 에서 OTG 로 옮긴다. 이 순간 COM 이 사라진다. */
    usb_phy_config_t cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
    };
    if (usb_new_phy(&cfg, &s_phy) != ESP_OK) {
        ESP_LOGE(TAG, "PHY 를 못 가져왔다");
        return false;
    }

    s_active = true;
    if (xTaskCreatePinnedToCore(usb_task, "usb_msc", 4096, NULL, 5, &s_task, 0) != pdPASS) {
        s_active = false;
        usb_del_phy(s_phy); s_phy = NULL;
        return false;
    }
    return true;
}

void usb_msc_stop(void)
{
    if (!s_active) return;
    /* 먼저 "매체가 빠졌다" 고 알리고 잠깐 둔다. 호스트가 그걸 읽을 틈을
     * 안 주고 선을 끊으면 윈도우가 "장치를 제거하지 않고 뽑았다" 고 짖는다. */
    s_ejected = true;
    vTaskDelay(pdMS_TO_TICKS(400));
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "COM 으로 되돌아간다 (재부팅)");
    esp_restart();
}

bool usb_msc_active(void)  { return s_active; }
bool usb_msc_mounted(void) { return s_active && tud_mounted(); }
bool usb_msc_ejected(void) { return s_ejected; }
