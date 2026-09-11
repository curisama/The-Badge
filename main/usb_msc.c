/* USB storage mode — presenting the made-up FAT to a host.
 *
 * This file is only the wiring. Building the volume is entirely usb_export.c's
 * job; this answers what TinyUSB asks: how many sectors, give me this sector,
 * can I write.
 *
 * 🚨 **Read-only, deliberately.** Leave writing open and Windows tries to
 * create `System Volume Information` the moment it attaches. Our volume is
 * made up on the fly and has nowhere to put a write. Declaring the medium
 * write-protected makes Windows accept that and create nothing — quieter than
 * refusing writes.
 *
 * 🚨 **Leaving means rebooting.** TinyUSB 0.15 has no teardown (`tusb_teardown`
 * exists only as a comment in the header). Taking the PHY back from OTG to
 * USB-Serial/JTAG is a path nobody has verified, and half-reverted it leaves
 * no serial port and no way to flash. A reboot takes two seconds and is
 * certain. Going the other way is different — handing the PHY to OTG is what
 * esp_tinyusb does routinely, so that happens in place.
 *
 * 🚨 It cannot be bricked. However it fails, the ROM bootloader always comes
 * up as USB-Serial/JTAG — BOOT held with RESET can always reflash it. */
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
static volatile bool    s_ejected;     /* the host ejected it */

/* ── descriptors ─────────────────────────────────────────────
 * 🚨 The VID is Espressif's (0x303A). Borrowing someone else's VID invites
 * their driver to attach. The PID uses the MSC slot in the 0x4000 range
 * Espressif keeps open for self-built devices. */
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

/* 🚨 A serial number is required. Without one, Windows treats every
 * connection as a new device and assigns a different drive letter each time. */
static const char *const s_desc_str[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: English (US) */
    "Badge",                        /* 1: manufacturer */
    "Badge Recorder",               /* 2: product */
    "BADGE0001",                    /* 3: serial */
    "Recordings",                   /* 4: the MSC interface */
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

/* 🚨 Answering false to "is there media" makes the host see an empty drive.
 * Pressing done on the badge uses this to **report the medium as removed**
 * first, and drops the line a moment later — which is what stops Windows
 * complaining. */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  /* no medium */
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

/* This is where "safely remove" in Windows arrives. */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition;
    if (load_eject && !start) {
        ESP_LOGI(TAG, "host ejected it — returning to serial");
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
    /* 🚨 The host may ask for something starting mid-sector. A whole sector is
     * generated and then cut from — generating half of one would mean two
     * copies of the boundary arithmetic. */
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
    return false;                    /* write-protected medium */
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1;                       /* never reached */
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;                    /* nothing to lock */
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/* ── running it ──────────────────────────────────────────── */
static void usb_task(void *arg)
{
    (void)arg;
    /* 🚨 tusb_init is called **inside this task**. It allocates an interrupt
     * (esp_intr_alloc), and interrupts bind to the core that allocated them.
     * Called elsewhere, the interrupt and the task handling it end up on
     * different cores. */
    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init failed");
        s_active = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "mounted as a drive — %d files, %lu sectors",
             usb_export_files(), (unsigned long)usb_export_sectors());
    while (s_active) tud_task();
    vTaskDelete(NULL);
}

bool usb_msc_start(void)
{
    if (s_active) return true;

    usb_export_build();
    s_ejected = false;

    /* Move the PHY from USB-Serial/JTAG to OTG. The serial port vanishes here. */
    usb_phy_config_t cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
    };
    if (usb_new_phy(&cfg, &s_phy) != ESP_OK) {
        ESP_LOGE(TAG, "could not take the PHY");
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
    /* Report "medium removed" first and wait a moment. Cutting the line
     * without giving the host a chance to read that makes Windows complain
     * about a device removed without ejecting. */
    s_ejected = true;
    vTaskDelay(pdMS_TO_TICKS(400));
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "returning to serial (rebooting)");
    esp_restart();
}

bool usb_msc_active(void)  { return s_active; }
bool usb_msc_mounted(void) { return s_active && tud_mounted(); }
bool usb_msc_ejected(void) { return s_ejected; }
