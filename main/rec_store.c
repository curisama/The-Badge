/* The recording store. The badge records on its own into flash.
 *
 * The point is using it in a meeting room with no phone and no WiFi, so
 * nothing is streamed. The original is always in flash first and getting it
 * off is a later problem — there is nothing to lose to a dropped connection.
 *
 * Partition layout (rec, 24 MB):
 *   0x0000..0x0FFF  one sector of directory
 *   0x1000..        data, appended in order
 *
 * There is no filesystem because audio is append-only: a FAT here would add
 * overhead and fragmentation and nothing else. */
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
    uint32_t off;        /* data start, relative to the partition */
    uint32_t bytes;      /* ADPCM bytes */
    int64_t  started;    /* unix seconds; 0 if the clock was not set */
    uint8_t  lang;       /* 0=ko 1=en */
    uint8_t  done;       /* did it close cleanly? */
    uint8_t  sent;       /* has it been taken off the device? */
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

/* State while recording */
static volatile bool s_active;
static volatile bool s_stop_req;
static int           s_slot = -1;
static uint32_t      s_wrote;        /* bytes written by this recording */
static uint32_t      s_erased_upto;  /* everything below this is already erased */
static uint8_t      *s_page;         /* written 4 KB at a time */
static int           s_page_len;
static int64_t       s_started_us;

/* ── the directory ──────────────────────────────────────── */

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
    if (!s_part) { ESP_LOGW(TAG, "no recording partition — the table needs reflashing"); return; }

    esp_partition_read(s_part, 0, &s_dir, sizeof s_dir);
    if (s_dir.magic != REC_MAGIC || s_dir.count > REC_MAX) {
    ESP_LOGI(TAG, "creating a fresh directory");
        memset(&s_dir, 0, sizeof s_dir);
        s_dir.magic = REC_MAGIC;
        dir_save();
    }
    ESP_LOGI(TAG, "recording area %lu KB, %lu stored, %d not yet taken off",
             (unsigned long)(s_part->size / 1024),
             (unsigned long)s_dir.count, port_rec_pending());
    /* 🚨 Swept once at boot too. Anything left from being switched off after
     * an export, or from before the deletion code existed, is flagged as sent
     * and still holding its space. */
    purge_sent();
}

/* Where the next data goes */
static uint32_t alloc_off(void)
{
    uint32_t end = DATA_START;
    for (uint32_t i = 0; i < s_dir.count; i++) {
        uint32_t e = s_dir.ent[i].off + s_dir.ent[i].bytes;
        if (e > end) end = e;
    }
    return (end + SECTOR - 1) & ~(SECTOR - 1);   /* rounded up to a sector */
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
    /* 🚨 Dividing (16000 / 505) first truncates 31.68 to 31 in integers,
     * which turns 8,111 bytes a second into 7,936 and makes the remaining
     * time 2.2% too generous (51.7 minutes in a 24 MB area showed as 52.9).
     * Multiply first to keep the digits. */
    uint64_t left = (uint64_t)(s_part->size - used) * ADPCM_BLOCK_SAMPLES;
    return (uint32_t)(left / ((uint64_t)ADPCM_BLOCK_BYTES * SAMPLE_RATE));
}

/* ── writing ────────────────────────────────────────────── */

static bool page_flush(void)
{
    if (!s_page_len) return true;
    uint32_t off = s_dir.ent[s_slot].off + s_wrote;

    /* The sector has to be erased before it can be written */
    uint32_t need = (off + s_page_len + SECTOR - 1) & ~(SECTOR - 1);
    if (need > s_erased_upto) {
        uint32_t from = s_erased_upto > off ? s_erased_upto : (off & ~(SECTOR - 1));
        if (esp_partition_erase_range(s_part, from, need - from) != ESP_OK) {
            ESP_LOGE(TAG, "erase failed @0x%lX", (unsigned long)from);
            return false;
        }
        s_erased_upto = need;
    }
    if (esp_partition_write(s_part, off, s_page, s_page_len) != ESP_OK) {
        ESP_LOGE(TAG, "write failed @0x%lX", (unsigned long)off);
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

/* ── the microphone ─────────────────────────────────────── */

/* The microphone handle. bsp_audio_codec_microphone_init() allocates i2c_ctrl,
 * es7210 and codec_dev afresh on every call, and the BSP offers no way to
 * undo that — so creating one per recording simply leaks. It is created once
 * and kept.
 * What actually consumes resources is open/close (the I2S channel), and that
 * is done per recording. */
static esp_codec_dev_handle_t s_mic;

static void mic_task(void *arg)
{
    (void)arg;
    bool s_opened = false;
    if (!s_mic) s_mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_handle_t mic = s_mic;
    if (!mic) {
        ESP_LOGE(TAG, "microphone init failed");
        goto bail;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE, .channel = 1, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "microphone open failed");
        goto bail;
    }
    s_opened = true;
    esp_codec_dev_set_in_gain(mic, 30.0f);

    const int CHUNK = ADPCM_BLOCK_SAMPLES;      /* 505 samples = one block */
    int16_t *pcm = heap_caps_malloc(CHUNK * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t  blk[ADPCM_BLOCK_BYTES];
    int64_t  last_dir = esp_timer_get_time();

    if (!pcm) { ESP_LOGE(TAG, "no buffer"); goto bail; }

    /* Samples go missing if the CPU sleeps while I2S is reading the microphone */
    port_pm_hold(true);
    ESP_LOGI(TAG, "recording started (slot %d)", s_slot);
    port_heap_report("rec-start");
    while (!s_stop_req) {
        if (esp_codec_dev_read(mic, pcm, CHUNK * 2) != ESP_OK) break;
        adpcm_encode_block(pcm, CHUNK, blk);
        if (!put_block(blk)) break;

        /* Rewrite the directory every 30 s so a sudden power loss keeps this much */
        int64_t now = esp_timer_get_time();
        if (now - last_dir > 30000000LL) {
            last_dir = now;
            s_dir.ent[s_slot].bytes = s_wrote;
            dir_save();
            port_battery_log("rec");
        }
        /* Stop by itself when the space runs out */
        if (s_dir.ent[s_slot].off + s_wrote + SECTOR >= s_part->size) {
            ESP_LOGW(TAG, "recording area full — stopping");
            break;
        }
    }

    page_flush();
    s_dir.ent[s_slot].bytes = s_wrote;
    s_dir.ent[s_slot].done  = 1;
    dir_save();

    esp_codec_dev_close(mic);       /* give the I2S channel back; the handle is reused */
    heap_caps_free(pcm);
    /* Recording finished, so give the 4 KB write buffer back too. Reallocated next time. */
    if (s_page) { heap_caps_free(s_page); s_page = NULL; s_page_len = 0; }
    ESP_LOGI(TAG, "recording ended, %lu bytes (%lu s)",
             (unsigned long)s_wrote,
             (unsigned long)(s_wrote / ((SAMPLE_RATE / ADPCM_BLOCK_SAMPLES) * ADPCM_BLOCK_BYTES)));
    port_pm_hold(false);
    s_active = false;
    port_heap_report("rec-end");     /* should match the start */
    vTaskDelete(NULL);
    return;

bail:
    /* Whatever failed, give everything back on the way out. The slot is
     * collapsed too, so an empty recording does not appear in the list —
     * otherwise zero-byte recordings pile up that can never be exported. */
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
    if (port_rec_free_seconds() < 60) { ESP_LOGW(TAG, "less than a minute of space left"); return false; }

    /* Push out the oldest first. Anything already taken off is expendable. */
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

    /* An open speaker codec fights over the same I2S. Take it down first. */
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

/* ── what the export side uses ─────────────────────────── */

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

/* ── taking exported recordings out of the list ───────────────
 * 🚨 Without this the space never comes back. alloc_off() finds the highest
 * end in the list and appends after it, so an exported recording still listed
 * keeps the watermark pinned there. That is why a 24 MB area had 3.9 minutes
 * left with all twelve recordings already taken off.
 *
 * The bytes are not erased — erasing 17 MB on the spot would hold the flash
 * for over forty seconds, and the next recording erases sector by sector just
 * before writing anyway. Leaving the list is what makes the space available.
 *
 * 🚨 Never called while recording. s_slot is an index into this array, so
 * removing an earlier entry makes the microphone write into the wrong one. */
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
    ESP_LOGI(TAG, "removed %u exported recordings — %lu s now available",
             (unsigned)gone, (unsigned long)port_rec_free_seconds());
}

void rec_mark_sent(int slot)
{
    if (slot < 0 || slot >= (int)s_dir.count) return;
    s_dir.ent[slot].sent = 1;
    dir_save();
    ESP_LOGI(TAG, "slot %d exported", slot);
    purge_sent();
}

/* ── the window used for exporting (rec_export.h) ─────────────
 * 🚨 Only these two are exposed, to keep the internal structure in. The USB
 * side only needs to know how many there are, where each starts and how long
 * it is. */
#include "rec_export.h"

int rec_export_list(rec_export_t *out, int max)
{
    if (!s_ready) return 0;
    int got = 0;
    for (uint32_t i = 0; i < s_dir.count && got < max; i++) {
        const rec_ent_t *e = &s_dir.ent[i];
        /* 🚨 Unfinished recordings are excluded. Their length is not settled,
         * and exporting one makes the host read past the end. */
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
