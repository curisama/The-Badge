/* BLE HID 마우스. ESP-IDF 의 esp_hid 를 쓰고, GAP 부분은 공식 예제의
 * esp_hid_gap.c 를 그대로 가져왔다(Unlicense/CC0).
 *
 * 리포트는 4바이트다: [버튼][dx][dy][휠]. 리포트 ID 없이 부트 마우스와
 * 같은 모양이라 붙는 쪽이 뭘 하든 그냥 마우스로 본다. */
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

/* 마우스 + 키보드 복합 리포트. 리포트 ID 로 구분한다.
 *   ID 1 = 마우스 4바이트  [버튼][dx][dy][휠]
 *   ID 2 = 키보드 8바이트  [수정키][0][키코드 x6]
 * 🚨 리포트 맵이 바뀌면 폰이 옛 모습을 캐시하고 있으므로 한 번 등록을 지우고
 *    다시 짝지어야 한다. */
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
    0x81, 0x02,        /*       Input (Data, Variable, Absolute) — 버튼 3개 */
    0x95, 0x01, 0x75, 0x05,
    0x81, 0x03,        /*       Input (Constant) — 남는 5비트 */
    0x05, 0x01,        /*       Usage Page (Generic Desktop) */
    0x09, 0x30,        /*       Usage (X) */
    0x09, 0x31,        /*       Usage (Y) */
    0x09, 0x38,        /*       Usage (Wheel) */
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,        /*       Input (Data, Variable, Relative) — 상대 이동 */
    0xC0,
    0xC0,

    /* ── 키보드 ── */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /*   Usage (Keyboard) */
    0xA1, 0x01,        /*   Collection (Application) */
    0x85, 0x02,        /*     Report ID (2) */
    0x05, 0x07,        /*     Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7,        /* Ctrl/Shift/Alt/GUI 좌우 8개 */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,        /*     Input — 수정키 비트 8개 */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x03,        /*     Input (Constant) — 예약 1바이트 */
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,        /*     Input — 동시에 누른 키 6개 */
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

/* esp_hid_gap.c 가 짝짓기(암호화)가 끝나면 이걸 부른다.
 * 예제에선 여기서 리포트 보내는 태스크를 띄우는데, 우리는 터치가 있을 때만
 * 보내므로 붙었다는 표시만 한다. */
/* 폰이 "이 숫자 맞냐"고 물을 때 그 숫자를 받아둔다.
 * 화면이 있는데 안 보여주면 사용자가 뭘 확인해야 할지 모른다. */
static volatile uint32_t s_passkey;
static volatile int64_t  s_passkey_at;

void badge_ble_passkey(uint32_t key)
{
    s_passkey = key;
    s_passkey_at = esp_timer_get_time();
}

uint32_t port_hid_passkey(void)
{
    /* 30초 지나면 지운다 */
    if (!s_passkey || esp_timer_get_time() - s_passkey_at > 30000000LL) return 0;
    return s_passkey;
}

/* 연결 간격을 짧게 요청한다.
 * 폰이 기본으로 잡는 값은 보통 30~50ms 라, 커서 좌표가 그 간격으로만 나간다.
 * 7.5~15ms 로 당기면 리포트가 서너 배 자주 나가 커서가 매끄러워진다.
 * 정하는 쪽은 폰이라 요청일 뿐이고, 거절당해도 동작엔 지장 없다. */
/* 폰이 허락한 연결 간격(1.25ms 단위). 0 = 아직 모름. */
static volatile uint16_t s_conn_int;
static bool              s_pm_held;   /* 라이트슬립을 잡고 있나 */

void badge_ble_set_interval(uint16_t units) { s_conn_int = units; }

int port_hid_interval_ms(void)
{
    if (!s_conn_int) return 15;                 /* 모르면 최악을 가정 */
    int ms = (s_conn_int * 125 + 50) / 100;     /* 1.25ms 단위 → ms 반올림 */
    return ms < 1 ? 1 : ms;
}

static uint8_t s_peer_addr[6];
static bool    s_peer_addr_ok;

/* 골라둔 호스트. 비어 있으면 아무나 받는다. */
static uint8_t s_want_addr[6];
static bool    s_want_set;

/* 이름표. 주소는 본딩이 들고 있으니 여기엔 이름만 담는다. */
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
    ESP_LOGI(TAG, "아무 기기나 받는다");
}

void port_hid_host_pick(const uint8_t addr[6])
{
    memcpy(s_want_addr, addr, 6);
    s_want_set = true;

    /* 🚨 순서가 중요하다. 화이트리스트를 먼저 바꾸고 끊어야, 끊긴 직후
     * 스택이 다시 광고할 때 이미 새 규칙이 서 있다. 거꾸로 하면 그 틈에
     * 옛 호스트가 다시 붙는다. */
    esp_ble_gap_clear_whitelist();
    esp_ble_gap_update_whitelist(true, (uint8_t *)addr, BLE_WL_ADDR_TYPE_PUBLIC);
    if (s_connected && s_peer_addr_ok && memcmp(s_peer_addr, addr, 6) != 0)
        esp_ble_gap_disconnect(s_peer_addr);
    ESP_LOGI(TAG, "이 기기에만 붙는다 %02X:%02X:%02X:%02X:%02X:%02X",
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
    /* 🚨 고른 호스트가 아니면 그 자리에서 끊는다. 화이트리스트만으로는
     * 스택 구현에 따라 새고, 이전 호스트는 끊겨도 집요하게 다시 붙는다.
     * 여기서 한 번 더 막는 게 확실하다. */
    if (s_want_set && memcmp(addr, s_want_addr, 6) != 0) {
        ESP_LOGW(TAG, "고른 기기가 아니다 — 끊는다");
        esp_ble_gap_disconnect(addr);
        return;
    }
    memcpy(s_peer_addr, addr, 6);
    s_peer_addr_ok = true;
    esp_ble_conn_update_params_t p = {
        .min_int = 6,      /* 6 x 1.25ms = 7.5ms */
        .max_int = 12,     /* 15ms */
        .latency = 0,
        .timeout = 400,    /* 4초 (10ms 단위) */
    };
    memcpy(p.bda, addr, sizeof(esp_bd_addr_t));
    esp_err_t r = esp_ble_gap_update_conn_params(&p);
    ESP_LOGI(TAG, "연결 간격 7.5~15ms 요청 (%s)", r == ESP_OK ? "보냄" : "실패");
}

void ble_hid_task_start_up(void)
{
    /* 짝짓기가 끝난 것이지 HID 로 붙은 게 아니다. 여기서 connected 로
     * 표시했더니, 폰이 곧바로 끊었는데도 계속 리포트를 쏴서 로그가 터졌다. */
    s_passkey = 0;
    ESP_LOGI(TAG, "짝짓기 완료");
}

static void hidd_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *p = (esp_hidd_event_data_t *)event_data;

    ESP_LOGI(TAG, "HIDD 이벤트 %d", (int)event);

    switch (event) {
    case ESP_HIDD_START_EVENT:
        /* 🚨 "한 번 붙었던 게 자동으로 안 붙는다" 를 가리려면 먼저 본딩이
         * 남아 있는지부터 알아야 한다. 0 이면 짝짓기가 날아간 것이고,
         * 남아 있는데 안 붙으면 광고 쪽 문제다. 부팅마다 찍어둔다. */
        ESP_LOGI(TAG, "짝지어둔 기기 %d대", esp_ble_get_bond_device_num());
        ESP_LOGI(TAG, "HID 서비스 등록 완료 — 광고 시작");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        snprintf(s_peer, sizeof(s_peer), "connected");
        ESP_LOGI(TAG, "붙었다");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_peer_addr_ok = false;      /* 끊겼으면 누구였는지도 잊는다 */
        snprintf(s_peer, sizeof(s_peer), "advertising");
        ESP_LOGI(TAG, "끊겼다 — 다시 광고");
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
    /* BLE 가 도는 동안엔 CPU 를 못 재운다. 자면 연결 간격을 놓쳐
     * 마우스가 뚝뚝 끊기거나 아예 끊어진다. */
    if (!s_pm_held) { port_pm_hold(true); s_pm_held = true; }
    if (s_inited) {
        if (!s_connected) esp_hid_ble_gap_adv_start();
        return;
    }
    /* 스택은 한 번만 올린다. 껐다 켜기를 반복하면 불안정하다. */
    if (esp_hid_gap_init(HIDD_BLE_MODE) != ESP_OK) {
        ESP_LOGE(TAG, "GAP 초기화 실패");
        return;
    }
    if (esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_MOUSE, s_cfg.device_name) != ESP_OK) {
        ESP_LOGE(TAG, "광고 설정 실패");
        return;
    }
    /* 이 줄이 빠져 있었다. HID 의 GATT 서비스는 Bluedroid 의 GATTS 콜백을
     * 통해 만들어진다 — 등록을 안 하면 서비스가 아예 안 생기고,
     * 폰은 정체 모를 BLE 기기로 보고 "전용 앱을 깔라"고 한다.
     * 우리 hidd 콜백도 안 불려서 연결/끊김을 영영 모른다. */
    /* 시각 창구가 GATTS 콜백을 대신 받아서 HID 것과 갈라 보낸다.
     * (Bluedroid 는 콜백을 하나만 받는다) */
    extern esp_err_t time_svc_register(void);
    if (time_svc_register() != ESP_OK) {
        ESP_LOGE(TAG, "GATTS 등록 실패");
        return;
    }
    if (esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "HID 장치 생성 실패");
        return;
    }
    s_inited = true;
    snprintf(s_peer, sizeof(s_peer), "advertising");
    ESP_LOGI(TAG, "BLE 올림 (%lld ms) — 내부 RAM %uKB",
             (esp_timer_get_time() - s_start_us) / 1000,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

/* BLE 스택은 내부 RAM 을 60~70KB 먹는다. 이 보드는 부팅 직후 내부 RAM 이
 * 125KB 뿐이라 그게 통째로 남아 있으면 다른 게 못 돌아간다.
 * 그래서 BLE 를 쓰는 앱을 나가면 스택을 통째로 내린다.
 * 짝짓기 키는 NVS 에 남으므로 다시 붙일 때 재등록은 필요 없다. */
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
    /* 🚨 블루투스만 내리고 HID GAP 계층을 안 내리면 그쪽 세마포어가 살아남아,
     * 다음에 켤 때 esp_hid_gap_init 이 "Already initialised" 로 거절한다.
     * = 한 번 끄면 마우스가 다시는 안 붙는다. deinit 이 아래 4가지를
     * 다 해주므로 직접 부르지 않는다. */
    esp_hid_gap_deinit();

    s_inited = false;
    s_connected = false;
    snprintf(s_peer, sizeof(s_peer), "off");
    ESP_LOGI(TAG, "BLE 내림 (%lld ms) — 내부 RAM %uKB",
             (esp_timer_get_time() - t0) / 1000,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

bool        port_hid_connected(void) { return s_connected; }
bool        port_hid_up(void)        { return s_inited; }

/* 짝지은 기기를 전부 잊는다.
 * 🚨 배지는 여러 기기를 기억한다. 그래서 폰에서 지울 필요는 없는데,
 * 배지가 광고를 시작하면 근처 폰이 먼저 낚아채 간다 — 다른 데 붙이려면
 * 폰 블루투스를 끄거나 여기서 기억을 지워야 한다. 지금까진 지울 방법이
 * 아예 없어서 한번 꼬이면 손쓸 데가 없었다. */
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
    ESP_LOGI(TAG, "짝지은 기기 %d개를 잊었다", n);
    return n;
}
const char *port_hid_peer(void)      { return s_peer; }

/* 키 하나를 눌렀다 뗀다. 수정키는 비트: 1=Ctrl 2=Shift 4=Alt 8=GUI (왼쪽) */
void port_hid_key(unsigned modifier, unsigned keycode)
{
    if (!s_dev || !s_connected) return;
    uint8_t rpt[8] = { (uint8_t)modifier, 0, (uint8_t)keycode, 0, 0, 0, 0, 0 };
    esp_hidd_dev_input_set(s_dev, 0, 2, rpt, sizeof(rpt));
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(rpt, 0, sizeof(rpt));
    esp_hidd_dev_input_set(s_dev, 0, 2, rpt, sizeof(rpt));
}

/* 아스키 → HID 키코드. 미국 배열 기준.
 * 폰 RDP 로 긴 비밀번호를 치는 게 고통이라 배지가 대신 친다. */
static bool ascii_to_key(char c, unsigned *mod, unsigned *code)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') { *code = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *code = 0x04 + (c - 'A'); *mod = 2; return true; }
    if (c >= '1' && c <= '9') { *code = 0x1E + (c - '1'); return true; }
    if (c == '0') { *code = 0x27; return true; }

    /* 수정키 없이 나오는 것: 스페이스 - = [ ] \\ ; ' ` , . / 엔터 탭 */
    static const char    plain_c[] = { ' ', '-', '=', '[', ']', '\\', ';', '\'',
                                       '`', ',', '.', '/', '\n', '\t' };
    static const uint8_t plain_k[] = { 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34,
                                       0x35, 0x36, 0x37, 0x38, 0x28, 0x2B };
    for (unsigned i = 0; i < sizeof(plain_k); i++) {
        if (c == plain_c[i]) { *code = plain_k[i]; return true; }
    }

    /* Shift 를 눌러야 나오는 것 */
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
        vTaskDelay(pdMS_TO_TICKS(12));      /* 너무 빠르면 받는 쪽이 흘린다 */
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
        /* 이벤트를 놓쳤더라도 여기서 알아챈다 */
        s_connected = false;
        snprintf(s_peer, sizeof(s_peer), "advertising");
        esp_hid_ble_gap_adv_start();
    }
}
