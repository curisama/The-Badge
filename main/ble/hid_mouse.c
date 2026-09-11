/* BLE HID mouse. Built on ESP-IDF's esp_hid, with the GAP half taken as-is
 * from the official example's esp_hid_gap.c (Unlicense/CC0).
 *
 * A mouse report is four bytes: [buttons][dx][dy][wheel]. No report ID, the
 * same shape as a boot mouse, so whatever it connects to simply sees a mouse. */
#include "port.h"
#include "esp_hid_gap.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "hid";

/* A composite mouse and keyboard report, separated by report ID.
 *   ID 1 = mouse, 4 bytes     [buttons][dx][dy][wheel]
 *   ID 2 = keyboard, 8 bytes  [modifiers][0][keycode x6]
 * 🚨 Changing this map means the host has the old one cached — the pairing
 *    has to be removed and redone once. */
static const unsigned char mouse_report_map[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /*   Usage (Mouse) */
    0xA1, 0x01,        /*   Collection (Application) */
    0x85, 0x01,        /*     Report ID (1) */
    0x09, 0x01,        /*     Usage (Pointer) */
    0xA1, 0x00,        /*     Collection (Physical) */
    0x05, 0x09,        /*       Usage Page (Buttons) */
    0x19, 0x01,        /*       Usage Minimum (1) */
    0x29, 0x03,        /*       Usage Maximum (3) */
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01,
    0x81, 0x02,        /*       Input (Data, Variable, Absolute) — three buttons */
    0x95, 0x01, 0x75, 0x05,
    0x81, 0x03,        /*       Input (Constant) — the remaining five bits */
    0x05, 0x01,        /*       Usage Page (Generic Desktop) */
    0x09, 0x30,        /*       Usage (X) */
    0x09, 0x31,        /*       Usage (Y) */
    0x09, 0x38,        /*       Usage (Wheel) */
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,        /*       Input (Data, Variable, Relative) — relative motion */
    0xC0,
    0xC0,

    /* ── keyboard ── */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /*   Usage (Keyboard) */
    0xA1, 0x01,        /*   Collection (Application) */
    0x85, 0x02,        /*     Report ID (2) */
    0x05, 0x07,        /*     Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7,        /* the eight Ctrl/Shift/Alt/GUI, left and right */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,        /*     Input — eight modifier bits */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x03,        /*     Input (Constant) — one reserved byte */
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,        /*     Input — six simultaneous keys */
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = mouse_report_map, .len = sizeof(mouse_report_map) },
};

static esp_hid_device_config_t s_cfg = {
    .vendor_id         = 0x16C0,
    .product_id        = 0x05DF,
    .version           = 0x0100,
    .device_name       = "Badge Mouse",
    .manufacturer_name = "amoled-badge",
    .serial_number     = "badge-1",
    .report_maps       = s_report_maps,
    .report_maps_len   = 1,
};

static esp_hidd_dev_t *s_dev;
static volatile bool   s_connected;
static bool            s_inited;
static char            s_peer[24] = "not paired";

/* esp_hid_gap.c calls this once pairing (encryption) completes.
 * The example starts a report-sending task here; we only send on touch, so
 * this just records that we are connected. */
/* Capture the number when the host asks "is this the same number?".
 * There is a screen, and not showing it leaves the user with nothing to compare. */
static volatile uint32_t s_passkey;
static volatile int64_t  s_passkey_at;

void badge_ble_passkey(uint32_t key)
{
    s_passkey = key;
    s_passkey_at = esp_timer_get_time();
}

uint32_t port_hid_passkey(void)
{
    /* Clear it after thirty seconds */
    if (!s_passkey || esp_timer_get_time() - s_passkey_at > 30000000LL) return 0;
    return s_passkey;
}

/* Ask for a shorter connection interval.
 * Hosts usually settle on 30-50 ms by default, which is how often cursor
 * coordinates can go out. Pulling that to 7.5-15 ms sends reports three or
 * four times as often and the cursor smooths out.
 * The host decides, so this is only a request; being refused changes nothing
 * functionally. */
/* The interval the host granted, in 1.25 ms units. 0 = not known yet. */
static volatile uint16_t s_conn_int;
static bool              s_pm_held;   /* are we holding light sleep off? */

void badge_ble_set_interval(uint16_t units) { s_conn_int = units; }

int port_hid_interval_ms(void)
{
    if (!s_conn_int) return 15;                 /* unknown — assume the worst */
    int ms = (s_conn_int * 125 + 50) / 100;     /* 1.25 ms units -> ms, rounded */
    return ms < 1 ? 1 : ms;
}

static uint8_t s_peer_addr[6];
static bool    s_peer_addr_ok;

/* The pinned host. Empty means accept anything. */
static uint8_t s_want_addr[6];
static bool    s_want_set;

/* Names. The addresses live in the bonding store, so only names are kept here. */
#define HOSTNM_NS  "badge"
#define HOSTNM_KEY "hostnm"
typedef struct { uint8_t addr[6]; char name[17]; uint8_t used; } hostnm_t;

static void hostnm_load(hostnm_t *t)
{
    memset(t, 0, sizeof(hostnm_t) * HID_HOSTS_MAX);
    nvs_handle_t nh;
    if (nvs_open(HOSTNM_NS, NVS_READONLY, &nh) != ESP_OK) return;
    size_t len = sizeof(hostnm_t) * HID_HOSTS_MAX;
    nvs_get_blob(nh, HOSTNM_KEY, t, &len);
    nvs_close(nh);
}

void port_hid_host_name_set(const uint8_t addr[6], const char *name)
{
    hostnm_t t[HID_HOSTS_MAX];
    hostnm_load(t);
    int slot = -1;
    for (int i = 0; i < HID_HOSTS_MAX; i++)
        if (t[i].used && memcmp(t[i].addr, addr, 6) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < HID_HOSTS_MAX; i++) if (!t[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;
    memcpy(t[slot].addr, addr, 6);
    snprintf(t[slot].name, sizeof t[slot].name, "%s", name ? name : "");
    t[slot].used = 1;

    nvs_handle_t nh;
    if (nvs_open(HOSTNM_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_blob(nh, HOSTNM_KEY, t, sizeof t);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

int port_hid_hosts(hid_host_t *out, int max)
{
    if (!s_inited) return 0;
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return 0;
    if (n > max) n = max;
    esp_ble_bond_dev_t *list = malloc(sizeof(esp_ble_bond_dev_t) * n);
    if (!list) return 0;
    int got = 0;
    if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
        hostnm_t t[HID_HOSTS_MAX];
        hostnm_load(t);
        for (int i = 0; i < n && got < max; i++) {
            memcpy(out[got].addr, list[i].bd_addr, 6);
            out[got].name[0] = '\0';
            for (int k = 0; k < HID_HOSTS_MAX; k++)
                if (t[k].used && memcmp(t[k].addr, list[i].bd_addr, 6) == 0) {
                    snprintf(out[got].name, sizeof out[got].name, "%s", t[k].name);
                    break;
                }
            out[got].here = s_connected && s_peer_addr_ok &&
                            memcmp(s_peer_addr, list[i].bd_addr, 6) == 0;
            got++;
        }
    }
    free(list);
    return got;
}

void port_hid_host_any(void)
{
    s_want_set = false;
    esp_ble_gap_clear_whitelist();
    ESP_LOGI(TAG, "accepting any device");
}

void port_hid_host_pick(const uint8_t addr[6])
{
    memcpy(s_want_addr, addr, 6);
    s_want_set = true;

    /* 🚨 The order matters. Change the whitelist *before* disconnecting, so
     * that when the stack re-advertises immediately afterwards the new rule
     * is already in place. The other way round lets the old host back in
     * through the gap. */
    esp_ble_gap_clear_whitelist();
    esp_ble_gap_update_whitelist(true, (uint8_t *)addr, BLE_WL_ADDR_TYPE_PUBLIC);
    if (s_connected && s_peer_addr_ok && memcmp(s_peer_addr, addr, 6) != 0)
        esp_ble_gap_disconnect(s_peer_addr);
    ESP_LOGI(TAG, "pinned to %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

bool port_hid_peer_addr(uint8_t out[6])
{
    if (!s_peer_addr_ok) return false;
    memcpy(out, s_peer_addr, 6);
    return true;
}

void badge_ble_bonded(esp_bd_addr_t addr)
{
    /* 🚨 Anything that is not the pinned host is dropped on the spot. A
     * whitelist alone leaks depending on the stack, and a previous host
     * reconnects persistently after being dropped. This second check is what
     * makes it certain. */
    if (s_want_set && memcmp(addr, s_want_addr, 6) != 0) {
        ESP_LOGW(TAG, "not the pinned device — dropping it");
        esp_ble_gap_disconnect(addr);
        return;
    }
    memcpy(s_peer_addr, addr, 6);
    s_peer_addr_ok = true;
    esp_ble_conn_update_params_t p = {
        .min_int = 6,      /* 6 x 1.25ms = 7.5ms */
        .max_int = 12,     /* 15ms */
        .latency = 0,
        .timeout = 400,    /* 4 s (in 10 ms units) */
    };
    memcpy(p.bda, addr, sizeof(esp_bd_addr_t));
    esp_err_t r = esp_ble_gap_update_conn_params(&p);
    ESP_LOGI(TAG, "requested a 7.5-15 ms interval (%s)", r == ESP_OK ? "sent" : "failed");
}

void ble_hid_task_start_up(void)
{
    /* Pairing finished is not the same as being connected over HID. Marking
     * connected here meant reports kept going out after the host had already
     * disconnected, and the log filled up. */
    s_passkey = 0;
    ESP_LOGI(TAG, "pairing complete");
}

static void hidd_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *p = (esp_hidd_event_data_t *)event_data;

    ESP_LOGI(TAG, "HIDD event %d", (int)event);

    switch (event) {
    case ESP_HIDD_START_EVENT:
        /* 🚨 To tell apart "it used to reconnect and now does not", the first
         * thing to know is whether the bond still exists. Zero means the
         * pairing is gone; a bond that exists but will not connect is an
         * advertising problem. Logged every boot. */
        ESP_LOGI(TAG, "%d bonded devices", esp_ble_get_bond_device_num());
        ESP_LOGI(TAG, "HID service registered — advertising");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        snprintf(s_peer, sizeof(s_peer), "connected");
        ESP_LOGI(TAG, "connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_peer_addr_ok = false;      /* disconnected, so forget who it was */
        snprintf(s_peer, sizeof(s_peer), "advertising");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_STOP_EVENT:
        s_connected = false;
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
    case ESP_HIDD_CONTROL_EVENT:
    case ESP_HIDD_OUTPUT_EVENT:
    case ESP_HIDD_FEATURE_EVENT:
    default:
        (void)p;
        break;
    }
}

void port_hid_start(void)
{
    static int64_t s_start_us;
    s_start_us = esp_timer_get_time();
    /* The CPU must not sleep while BLE is up. Sleeping makes it miss the
     * connection interval, so the mouse stutters or drops entirely. */
    if (!s_pm_held) { port_pm_hold(true); s_pm_held = true; }
    if (s_inited) {
        if (!s_connected) esp_hid_ble_gap_adv_start();
        return;
    }
    /* Bring the stack up once. Repeated up and down is unstable. */
    if (esp_hid_gap_init(HIDD_BLE_MODE) != ESP_OK) {
        ESP_LOGE(TAG, "GAP init failed");
        return;
    }
    if (esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE, s_cfg.device_name) != ESP_OK) {
        ESP_LOGE(TAG, "advertising setup failed");
        return;
    }
    /* This line was missing. HID's GATT service is created through Bluedroid's
     * GATTS callback — without registering it the service is never created at
     * all, and the host sees an unidentifiable BLE device and suggests
     * installing a dedicated app.
     * Our hidd callback is never called either, so connects and disconnects
     * go unnoticed forever. */
    /* The time service takes the GATTS callback and splits HID's off to it.
     * (Bluedroid accepts only one.) */
    extern esp_err_t time_svc_register(void);
    if (time_svc_register() != ESP_OK) {
        ESP_LOGE(TAG, "GATTS registration failed");
        return;
    }
    if (esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "HID device creation failed");
        return;
    }
    s_inited = true;
    snprintf(s_peer, sizeof(s_peer), "advertising");
    ESP_LOGI(TAG, "BLE up (%lld ms) — internal RAM %uKB",
             (esp_timer_get_time() - s_start_us) / 1000,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

/* The BLE stack takes 60-70 KB of internal RAM. This board has 125 KB free
 * just after boot, so leaving that claimed starves everything else.
 * So leaving a BLE app takes the whole stack down again.
 * The pairing keys live in NVS, so reconnecting needs no re-registration. */
void port_hid_stop(void)
{
    if (s_pm_held) { port_pm_hold(false); s_pm_held = false; }
    if (!s_inited) return;

    int64_t t0 = esp_timer_get_time();
    esp_ble_gap_stop_advertising();
    if (s_dev) {
        esp_hidd_dev_deinit(s_dev);
        s_dev = NULL;
    }
    /* 🚨 Taking Bluetooth down without taking the HID GAP layer down leaves
     * its semaphore alive, and the next esp_hid_gap_init refuses with
     * "Already initialised" — meaning the mouse never connects again once it
     * has been switched off. deinit does all four steps, so they are not
     * called individually. */
    esp_hid_gap_deinit();

    s_inited = false;
    s_connected = false;
    snprintf(s_peer, sizeof(s_peer), "off");
    ESP_LOGI(TAG, "BLE down (%lld ms) — internal RAM %uKB",
             (esp_timer_get_time() - t0) / 1000,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

bool        port_hid_connected(void) { return s_connected; }
bool        port_hid_up(void)        { return s_inited; }

/* Forget every paired device.
 * 🚨 The badge remembers several hosts, so there is no need to unpair from
 * the phone — but when the badge starts advertising, a nearby phone grabs it
 * first. Connecting it to something else means either turning that phone's
 * Bluetooth off or clearing the memory here. Until this existed there was no
 * way out of that at all. */
int port_hid_forget_all(void)
{
    if (!s_inited) return -1;
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return 0;
    esp_ble_bond_dev_t *list = malloc(sizeof(esp_ble_bond_dev_t) * n);
    if (!list) return -1;
    if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    }
    free(list);
    ESP_LOGI(TAG, "forgot %d paired devices", n);
    return n;
}
const char *port_hid_peer(void)      { return s_peer; }

/* Press and release one key. Modifier bits: 1=Ctrl 2=Shift 4=Alt 8=GUI (left) */
void port_hid_key(unsigned modifier, unsigned keycode)
{
    if (!s_dev || !s_connected) return;
    uint8_t rpt[8] = { (uint8_t)modifier, 0, (uint8_t)keycode, 0, 0, 0, 0, 0 };
    esp_hidd_dev_input_set(s_dev, 0, 2, rpt, sizeof(rpt));
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(rpt, 0, sizeof(rpt));
    esp_hidd_dev_input_set(s_dev, 0, 2, rpt, sizeof(rpt));
}

/* ASCII to HID keycodes, US layout.
 * Typing a long password over phone RDP is miserable, so the badge types it. */
static bool ascii_to_key(char c, unsigned *mod, unsigned *code)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') { *code = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *code = 0x04 + (c - 'A'); *mod = 2; return true; }
    if (c >= '1' && c <= '9') { *code = 0x1E + (c - '1'); return true; }
    if (c == '0') { *code = 0x27; return true; }

    /* Unshifted: space - = [ ] \\ ; ' ` , . / enter tab */
    static const char    plain_c[] = { ' ', '-', '=', '[', ']', '\\', ';', '\'',
                                       '`', ',', '.', '/', '\n', '\t' };
    static const uint8_t plain_k[] = { 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34,
                                       0x35, 0x36, 0x37, 0x38, 0x28, 0x2B };
    for (unsigned i = 0; i < sizeof(plain_k); i++) {
        if (c == plain_c[i]) { *code = plain_k[i]; return true; }
    }

    /* The ones that need Shift */
    static const char    shift_c[] = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
                                       '_', '+', '{', '}', '|', ':', '"', '~', '<', '>', '?' };
    static const uint8_t shift_k[] = { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
                                       0x26, 0x27, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33,
                                       0x34, 0x35, 0x36, 0x37, 0x38 };
    for (unsigned i = 0; i < sizeof(shift_k); i++) {
        if (c == shift_c[i]) { *code = shift_k[i]; *mod = 2; return true; }
    }
    return false;
}

void port_hid_type(const char *str)
{
    if (!s_dev || !s_connected || !str) return;
    for (const char *p = str; *p; p++) {
        unsigned mod, code;
        if (!ascii_to_key(*p, &mod, &code)) continue;
        port_hid_key(mod, code);
        vTaskDelay(pdMS_TO_TICKS(12));      /* too fast and the receiving end drops them */
    }
}

static int clamp8(int v) { return v < -127 ? -127 : (v > 127 ? 127 : v); }

void port_hid_mouse(int dx, int dy, unsigned buttons, int wheel)
{
    if (!s_dev || !s_connected) return;
    uint8_t rpt[4] = {
        (uint8_t)(buttons & 0x07),
        (uint8_t)(int8_t)clamp8(dx),
        (uint8_t)(int8_t)clamp8(dy),
        (uint8_t)(int8_t)clamp8(wheel),
    };
    if (esp_hidd_dev_input_set(s_dev, 0, 1, rpt, sizeof(rpt)) != ESP_OK) {
        /* Catch it here even if the event was missed */
        s_connected = false;
        snprintf(s_peer, sizeof(s_peer), "advertising");
        esp_hid_ble_gap_adv_start();
    }
}
