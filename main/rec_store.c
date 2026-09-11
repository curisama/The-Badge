/* 녹음 저장소. 배지가 혼자 녹음해서 플래시에 쌓고, WiFi 가 잡히면 올린다.
 *
 * 폰도 WiFi 도 없는 회의실에서 쓰는 게 목적이라 스트리밍을 안 한다.
 * 원본이 항상 플래시에 먼저 있고 전송은 나중 일이다 — 끊겨서 날아갈 게 없다.
 *
 * 파티션 배치 (rec, 24MB):
 *   0x0000..0x0FFF  디렉터리 한 섹터
 *   0x1000..        데이터. 순서대로 붙여 쓴다.
 *
 * 파일시스템을 안 얹은 이유: 오디오는 append 뿐이라 FAT 을 올려봐야
 * 오버헤드와 조각만 는다. */
#include "port.h"
#include "adpcm.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "rec";

#define REC_SUBTYPE   0x40
#define DIR_BYTES     4096
#define DATA_START    4096
#define SECTOR        4096
#define SAMPLE_RATE   16000
#define REC_MAGIC     0x43455242u      /* "BREC" */
#define REC_MAX       12

typedef struct {
    uint32_t off;        /* 파티션 기준 데이터 시작 */
    uint32_t bytes;      /* ADPCM 바이트 */
    int64_t  started;    /* unix 초. 시계가 안 맞았으면 0 */
    uint8_t  lang;       /* 0=ko 1=en */
    uint8_t  done;       /* 정상 종료됐나 */
    uint8_t  sent;       /* 올렸나 */
    uint8_t  pad;
} rec_ent_t;

typedef struct {
    uint32_t  magic;
    uint32_t  count;
    rec_ent_t ent[REC_MAX];
} rec_dir_t;

static const esp_partition_t *s_part;
static rec_dir_t  s_dir;
static bool       s_ready;

/* 녹음 중 상태 */
static volatile bool s_active;
static volatile bool s_stop_req;
static int           s_slot = -1;
static uint32_t      s_wrote;        /* 이번 녹음이 쓴 바이트 */
static uint32_t      s_erased_upto;  /* 여기까지는 지워져 있다 */
static uint8_t      *s_page;         /* 4KB 모아서 쓴다 */
static int           s_page_len;
static int64_t       s_started_us;

/* ── 디렉터리 ───────────────────────────────────────────── */

static void dir_save(void)
{
    if (!s_part) return;
    if (esp_partition_erase_range(s_part, 0, DIR_BYTES) != ESP_OK) return;
    esp_partition_write(s_part, 0, &s_dir, sizeof s_dir);
}

static void purge_sent(void);

static void dir_load(void)
{
    if (s_ready) return;
    s_ready = true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, REC_SUBTYPE, "rec");
    if (!s_part) { ESP_LOGW(TAG, "녹음 칸 없음 — 파티션을 다시 구워야 한다"); return; }

    esp_partition_read(s_part, 0, &s_dir, sizeof s_dir);
    if (s_dir.magic != REC_MAGIC || s_dir.count > REC_MAX) {
        ESP_LOGI(TAG, "디렉터리 새로 만든다");
        memset(&s_dir, 0, sizeof s_dir);
        s_dir.magic = REC_MAGIC;
        dir_save();
    }
    ESP_LOGI(TAG, "녹음 칸 %luKB · %lu건 보관 · 안 올린 것 %d건",
             (unsigned long)(s_part->size / 1024),
             (unsigned long)s_dir.count, port_rec_pending());
    /* 🚨 부팅 때도 한 번 훑는다. 올리고 나서 껐거나, 지우는 코드가 없던
     * 시절에 쌓인 것들은 올림 표시만 달고 자리를 물고 있다. */
    purge_sent();
}

/* 데이터를 이어 붙일 자리 */
static uint32_t alloc_off(void)
{
    uint32_t end = DATA_START;
    for (uint32_t i = 0; i < s_dir.count; i++) {
        uint32_t e = s_dir.ent[i].off + s_dir.ent[i].bytes;
        if (e > end) end = e;
    }
    return (end + SECTOR - 1) & ~(SECTOR - 1);   /* 섹터 경계로 올림 */
}

size_t port_rec_capacity(void)
{
    dir_load();
    return s_part ? s_part->size : 0;
}

int port_rec_pending(void)
{
    dir_load();
    int n = 0;
    for (uint32_t i = 0; i < s_dir.count; i++)
        if (s_dir.ent[i].done && !s_dir.ent[i].sent) n++;
    return n;
}

uint32_t port_rec_free_seconds(void)
{
    dir_load();
    if (!s_part) return 0;
    uint32_t used = alloc_off();
    if (used >= s_part->size) return 0;
    /* 🚨 (16000 / 505) 를 먼저 나누면 정수라 31.68 이 31 로 잘린다. 초당
     * 8,111바이트를 7,936 으로 보게 되어 남은 시간이 2.2% 넉넉하게 나왔다
     * (24MB 칸에서 51.7분인데 52.9분으로 떴다 — 0909 제보로 확인).
     * 곱하기를 먼저 해서 자릿수를 지킨다. */
    uint64_t left = (uint64_t)(s_part->size - used) * ADPCM_BLOCK_SAMPLES;
    return (uint32_t)(left / ((uint64_t)ADPCM_BLOCK_BYTES * SAMPLE_RATE));
}

/* ── 쓰기 ───────────────────────────────────────────────── */

static bool page_flush(void)
{
    if (!s_page_len) return true;
    uint32_t off = s_dir.ent[s_slot].off + s_wrote;

    /* 쓰기 전에 그 섹터가 지워져 있어야 한다 */
    uint32_t need = (off + s_page_len + SECTOR - 1) & ~(SECTOR - 1);
    if (need > s_erased_upto) {
        uint32_t from = s_erased_upto > off ? s_erased_upto : (off & ~(SECTOR - 1));
        if (esp_partition_erase_range(s_part, from, need - from) != ESP_OK) {
            ESP_LOGE(TAG, "지우기 실패 @0x%lX", (unsigned long)from);
            return false;
        }
        s_erased_upto = need;
    }
    if (esp_partition_write(s_part, off, s_page, s_page_len) != ESP_OK) {
        ESP_LOGE(TAG, "쓰기 실패 @0x%lX", (unsigned long)off);
        return false;
    }
    s_wrote += s_page_len;
    s_page_len = 0;
    return true;
}

static bool put_block(const uint8_t *b)
{
    memcpy(s_page + s_page_len, b, ADPCM_BLOCK_BYTES);
    s_page_len += ADPCM_BLOCK_BYTES;
    if (s_page_len >= SECTOR) return page_flush();
    return true;
}

/* ── 마이크 ─────────────────────────────────────────────── */

/* 마이크 핸들. bsp_audio_codec_microphone_init() 은 부를 때마다 i2c_ctrl +
 * es7210 + codec_dev 를 새로 잡는데 BSP 가 그걸 되돌려 지울 방법을 안 준다.
 * 그래서 녹음마다 만들면 그대로 샌다 — 한 번 만들어 계속 쓴다.
 * 실제로 자원을 먹는 건 open/close 쪽(I2S 채널)이라 그건 녹음마다 여닫는다. */
static esp_codec_dev_handle_t s_mic;

static void mic_task(void *arg)
{
    (void)arg;
    bool s_opened = false;
    if (!s_mic) s_mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_handle_t mic = s_mic;
    if (!mic) {
        ESP_LOGE(TAG, "마이크 초기화 실패");
        goto bail;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE, .channel = 1, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "마이크 열기 실패");
        goto bail;
    }
    s_opened = true;
    esp_codec_dev_set_in_gain(mic, 30.0f);

    const int CHUNK = ADPCM_BLOCK_SAMPLES;      /* 505 샘플 = 블록 하나 */
    int16_t *pcm = heap_caps_malloc(CHUNK * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t  blk[ADPCM_BLOCK_BYTES];
    int64_t  last_dir = esp_timer_get_time();

    if (!pcm) { ESP_LOGE(TAG, "버퍼 없음"); goto bail; }

    /* I2S 로 마이크를 읽는 동안 CPU 가 자면 샘플이 빈다 */
    port_pm_hold(true);
    ESP_LOGI(TAG, "녹음 시작 (슬롯 %d)", s_slot);
    port_heap_report("rec-start");
    while (!s_stop_req) {
        if (esp_codec_dev_read(mic, pcm, CHUNK * 2) != ESP_OK) break;
        adpcm_encode_block(pcm, CHUNK, blk);
        if (!put_block(blk)) break;

        /* 갑자기 꺼져도 여기까지는 살아남게 30초마다 디렉터리를 고쳐 쓴다 */
        int64_t now = esp_timer_get_time();
        if (now - last_dir > 30000000LL) {
            last_dir = now;
            s_dir.ent[s_slot].bytes = s_wrote;
            dir_save();
            port_battery_log("rec");
        }
        /* 남은 자리가 없으면 스스로 멈춘다 */
        if (s_dir.ent[s_slot].off + s_wrote + SECTOR >= s_part->size) {
            ESP_LOGW(TAG, "녹음 칸 참 — 멈춘다");
            break;
        }
    }

    page_flush();
    s_dir.ent[s_slot].bytes = s_wrote;
    s_dir.ent[s_slot].done  = 1;
    dir_save();

    esp_codec_dev_close(mic);       /* I2S 채널 반납. 핸들 자체는 재사용 */
    heap_caps_free(pcm);
    /* 녹음이 끝났으면 쓰기용 4KB 도 돌려준다. 다음 녹음 때 다시 잡는다. */
    if (s_page) { heap_caps_free(s_page); s_page = NULL; s_page_len = 0; }
    ESP_LOGI(TAG, "녹음 끝 %lu바이트 (%lu초)",
             (unsigned long)s_wrote,
             (unsigned long)(s_wrote / ((SAMPLE_RATE / ADPCM_BLOCK_SAMPLES) * ADPCM_BLOCK_BYTES)));
    port_pm_hold(false);
    s_active = false;
    port_heap_report("rec-end");     /* 시작 때와 같아야 정상이다 */
    vTaskDelete(NULL);
    return;

bail:
    /* 어디서 실패했든 잡은 건 다 돌려주고 나간다. 빈 녹음이 목록에
     * 남지 않게 슬롯도 접는다 — 안 그러면 못 올릴 0바이트가 쌓인다. */
    if (s_opened) esp_codec_dev_close(mic);
    if (s_page) { heap_caps_free(s_page); s_page = NULL; s_page_len = 0; }
    if (s_slot >= 0 && s_dir.count > 0 && s_slot == (int)s_dir.count - 1) {
        s_dir.count--;
        dir_save();
    }
    s_active = false;
    vTaskDelete(NULL);
}

bool port_rec_start(int lang)
{
    dir_load();
    if (!s_part || s_active) return false;
    if (port_rec_free_seconds() < 60) { ESP_LOGW(TAG, "남은 자리가 1분도 안 된다"); return false; }

    /* 오래된 것부터 밀어낸다. 이미 올린 건 버려도 된다. */
    if (s_dir.count >= REC_MAX) {
        memmove(&s_dir.ent[0], &s_dir.ent[1], sizeof(rec_ent_t) * (REC_MAX - 1));
        s_dir.count = REC_MAX - 1;
    }
    if (!s_page) s_page = heap_caps_malloc(SECTOR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_page) return false;

    s_slot = s_dir.count++;
    s_dir.ent[s_slot] = (rec_ent_t){
        .off = alloc_off(), .bytes = 0, .started = (int64_t)time(NULL),
        .lang = (uint8_t)(lang ? 1 : 0), .done = 0, .sent = 0,
    };
    s_wrote = 0;
    s_page_len = 0;
    s_erased_upto = s_dir.ent[s_slot].off;
    s_started_us = esp_timer_get_time();
    dir_save();

    /* 스피커 코덱이 열려 있으면 같은 I2S 를 두고 다툰다. 먼저 내린다. */
    port_tone_enable(false);

    s_stop_req = false;
    s_active = true;
    if (xTaskCreate(mic_task, "mic", 4096, NULL, 8, NULL) != pdPASS) {
        s_active = false;
        return false;
    }
    return true;
}

void port_rec_stop(void) { s_stop_req = true; }
bool port_rec_active(void) { return s_active; }

uint32_t port_rec_seconds(void)
{
    if (!s_active) return 0;
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}

/* ── 업로드 쪽에서 쓰는 것 ─────────────────────────────── */

const esp_partition_t *rec_partition(void) { dir_load(); return s_part; }

bool rec_oldest_unsent(uint32_t *off, uint32_t *bytes, int64_t *started, int *slot)
{
    dir_load();
    for (uint32_t i = 0; i < s_dir.count; i++) {
        if (s_dir.ent[i].done && !s_dir.ent[i].sent) {
            *off = s_dir.ent[i].off; *bytes = s_dir.ent[i].bytes;
            *started = s_dir.ent[i].started; *slot = (int)i;
            return true;
        }
    }
    return false;
}

/* ── 올린 것은 목록에서 뺀다 ──────────────────────────────────
 * 🚨 이게 없으면 자리가 영영 안 돌아온다. alloc_off() 는 목록에서 제일
 * 높은 끝을 찾아 그 뒤에 붙이는데, 올린 녹음이 목록에 남아 있으면 물꼬가
 * 거기 박힌 채다. 0908 에 24MB 칸이 12건 전부 올렸는데도 3.9분만 남아
 * 있었던 게 이 때문이다.
 *
 * 바이트를 지우진 않는다 — 17MB 를 그 자리에서 지우면 40초 넘게 플래시를
 * 물고 있어야 하고, 어차피 새로 녹음할 때 쓰기 직전에 섹터째 지운다.
 * 목록에서 빠지는 순간 그 자리는 다음 녹음이 덮어쓸 땅이 된다.
 *
 * 🚨 녹음 중에는 절대 부르지 않는다. s_slot 이 이 배열의 번호라서,
 * 앞의 항목이 빠지면 마이크가 엉뚱한 칸에 쓴다. */
static void purge_sent(void)
{
    if (s_active) return;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < s_dir.count; i++) {
        if (s_dir.ent[i].done && s_dir.ent[i].sent) continue;
        if (keep != i) s_dir.ent[keep] = s_dir.ent[i];
        keep++;
    }
    if (keep == s_dir.count) return;
    uint32_t gone = s_dir.count - keep;
    s_dir.count = keep;
    memset(&s_dir.ent[keep], 0, sizeof(rec_ent_t) * (REC_MAX - keep));
    dir_save();
    ESP_LOGI(TAG, "올린 녹음 %u건 지웠다 — 이제 %lu초 쓸 수 있다",
             (unsigned)gone, (unsigned long)port_rec_free_seconds());
}

void rec_mark_sent(int slot)
{
    if (slot < 0 || slot >= (int)s_dir.count) return;
    s_dir.ent[slot].sent = 1;
    dir_save();
    ESP_LOGI(TAG, "슬롯 %d 업로드 완료", slot);
    purge_sent();
}

/* ── 밖으로 내보낼 때 쓰는 창구 (rec_export.h) ─────────────────
 * 🚨 안쪽 구조를 밖으로 흘리지 않으려고 이 둘만 연다. USB 로 내보내는 쪽은
 * "몇 개가 있고 각각 어디서 시작해 몇 바이트인가" 만 알면 된다. */
#include "rec_export.h"

int rec_export_list(rec_export_t *out, int max)
{
    if (!s_ready) return 0;
    int got = 0;
    for (uint32_t i = 0; i < s_dir.count && got < max; i++) {
        const rec_ent_t *e = &s_dir.ent[i];
        /* 🚨 끝나지 않은 녹음은 뺀다. 길이가 아직 안 정해져서 내보내면
         * 호스트가 엉뚱한 데까지 읽는다. */
        if (!e->done || e->bytes == 0) continue;
        out[got].off     = e->off;
        out[got].bytes   = e->bytes;
        out[got].started = e->started;
        got++;
    }
    return got;
}

bool rec_export_read(uint32_t off, void *dst, uint32_t len)
{
    if (!s_part || !dst || !len) return false;
    if (off + len > s_part->size) return false;
    return esp_partition_read(s_part, off, dst, len) == ESP_OK;
}
