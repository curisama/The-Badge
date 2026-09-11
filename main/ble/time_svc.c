/* 시각 맞추기용 GATT 창구.
 *
 * 와이파이 없는 데서도 폰으로 시계를 맞추려고 만들었다. BLE 표준의
 * Current Time Service 는 아이폰만 서버로 내주고 안드로이드는 안 준다.
 * 그래서 반대로 우리가 "여기에 시각을 써 넣어라"는 창구를 연다.
 *
 *   서비스   0000ba5e-...
 *   특성     0000ba71-...  쓰기 12바이트
 *            [0..7]  int64 리틀엔디안, 1970 기준 초 (UTC)
 *            [8..11] int32 리틀엔디안, 시간대 오프셋(분). 예: 서울 = 540
 *   8바이트만 써도 된다 — 그럼 시간대는 그대로 둔다.
 *
 * Bluedroid 의 GATTS 콜백은 전역에 하나뿐이라, esp_hid 것과 우리 것을
 * 여기서 갈라 보낸다. 안 그러면 HID 서비스가 통째로 죽는다. */
#include "port.h"
#include "esp_gatts_api.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "timesvc";

#define TIME_APP_ID   0x55
#define CHAR_VAL_LEN  12

static const uint8_t SVC_UUID[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5e, 0xba, 0x00, 0x00,
};
static const uint8_t CHR_UUID[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x71, 0xba, 0x00, 0x00,
};
/* 회의 제어 ba72. 배지가 notify 로 명령을 쏘고, 앱이 write 로 결과를 되쏜다. */
static const uint8_t MEET_UUID[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x72, 0xba, 0x00, 0x00,
};

static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static uint16_t      s_svc_handle;
static uint16_t      s_chr_handle;
static uint16_t      s_meet_handle;
static uint16_t      s_meet_cccd;
static uint16_t      s_conn_id;
static bool          s_connected;
static bool          s_subscribed;   /* 앱이 notify 를 켰나 */
static uint8_t       s_ack;          /* 앱이 보낸 마지막 회신. 0 = 없음 */

static void apply_time(const uint8_t *v, int len)
{
    if (len < 8) return;

    int64_t secs = 0;
    memcpy(&secs, v, 8);
    if (secs < 1700000000LL) {            /* 2023 년보다 이르면 쓰레기 */
        ESP_LOGW(TAG, "말도 안 되는 시각 %lld — 무시", (long long)secs);
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)secs };
    settimeofday(&tv, NULL);

    if (len >= 12) {
        int32_t tz = 0;
        memcpy(&tz, v + 8, 4);
        if (tz >= -720 && tz <= 840) port_set_tz_offset(tz);
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "시각 맞춤: %04d-%02d-%02d %02d:%02d (UTC%+d분)",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, port_get_tz_offset());
}

static void time_gatts(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                       esp_ble_gatts_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        esp_gatt_srvc_id_t id = {
            .is_primary = true,
            .id = { .inst_id = 0, .uuid = { .len = ESP_UUID_LEN_128 } },
        };
        memcpy(id.id.uuid.uuid.uuid128, SVC_UUID, 16);
        esp_ble_gatts_create_service(gatts_if, &id, 8);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        s_svc_handle = p->create.service_handle;
        esp_ble_gatts_start_service(s_svc_handle);

        esp_bt_uuid_t cu = { .len = ESP_UUID_LEN_128 };
        memcpy(cu.uuid.uuid128, CHR_UUID, 16);
        esp_ble_gatts_add_char(s_svc_handle, &cu,
                               ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                               NULL, NULL);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
        /* 특성을 한 번에 둘 못 만든다. 시각 것이 끝나면 회의 것을 잇는다. */
        esp_bt_uuid_t u = p->add_char.char_uuid;
        if (u.len == ESP_UUID_LEN_128 && memcmp(u.uuid.uuid128, CHR_UUID, 16) == 0) {
            s_chr_handle = p->add_char.attr_handle;
            ESP_LOGI(TAG, "시각 창구 열림 (handle %d)", s_chr_handle);

            esp_bt_uuid_t mu = { .len = ESP_UUID_LEN_128 };
            memcpy(mu.uuid.uuid128, MEET_UUID, 16);
            esp_ble_gatts_add_char(s_svc_handle, &mu,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                                   ESP_GATT_CHAR_PROP_BIT_WRITE |
                                   ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                                   NULL, NULL);
        } else {
            s_meet_handle = p->add_char.attr_handle;
            /* notify 를 켜고 끄는 스위치(CCCD). 이게 없으면 앱이 구독을 못 한다. */
            esp_bt_uuid_t du = { .len = ESP_UUID_LEN_16,
                                 .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG } };
            esp_ble_gatts_add_char_descr(s_svc_handle, &du,
                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                         NULL, NULL);
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        s_meet_cccd = p->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "회의 창구 열림 (handle %d, cccd %d)", s_meet_handle, s_meet_cccd);
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = p->connect.conn_id;
        s_connected = true;
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_subscribed = false;
        break;

    case ESP_GATTS_WRITE_EVT:
        if (p->write.handle == s_chr_handle) {
            apply_time(p->write.value, p->write.len);
        } else if (p->write.handle == s_meet_cccd && p->write.len >= 2) {
            s_subscribed = (p->write.value[0] & 0x01) != 0;
            ESP_LOGI(TAG, "회의 구독 %s", s_subscribed ? "켬" : "끔");
        } else if (p->write.handle == s_meet_handle && p->write.len >= 1) {
            s_ack = p->write.value[0];
            ESP_LOGI(TAG, "앱 회신 0x%02X", s_ack);
        }
        if (p->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, p->write.conn_id,
                                        p->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;

    default:
        break;
    }
}

/* Bluedroid 는 GATTS 콜백을 하나만 받는다. HID 것과 우리 것을 갈라 보낸다. */
static void gatts_router(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                         esp_ble_gatts_cb_param_t *p)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (p->reg.app_id == TIME_APP_ID) {
            s_if = gatts_if;
            time_gatts(event, gatts_if, p);
        } else {
            esp_hidd_gatts_event_handler(event, gatts_if, p);
        }
        return;
    }
    if (s_if != ESP_GATT_IF_NONE && gatts_if == s_if) time_gatts(event, gatts_if, p);
    else                                              esp_hidd_gatts_event_handler(event, gatts_if, p);
}

esp_err_t time_svc_register(void)
{
    esp_err_t r = esp_ble_gatts_register_callback(gatts_router);
    if (r != ESP_OK) return r;
    return esp_ble_gatts_app_register(TIME_APP_ID);
}


/* ── 회의 버튼 창구 ───────────────────────────────────────── */

bool port_meet_link(void)
{
    return s_connected && s_subscribed;
}

bool port_meet_send(uint8_t cmd)
{
    if (!port_meet_link() || s_if == ESP_GATT_IF_NONE) return false;
    esp_err_t r = esp_ble_gatts_send_indicate(s_if, s_conn_id, s_meet_handle,
                                              1, &cmd, false /* notify */);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "명령 0x%02X 못 보냄 (%s)", cmd, esp_err_to_name(r));
        return false;
    }
    ESP_LOGI(TAG, "명령 0x%02X 보냄", cmd);
    return true;
}

bool port_meet_recv(uint8_t *status)
{
    if (!s_ack) return false;
    if (status) *status = s_ack;
    s_ack = 0;
    return true;
}
