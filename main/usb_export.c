/* Recordings as a USB drive — a read-only FAT that does not exist.
 *
 * Plug in and you get a serial port, as always. Press export and you get a
 * drive. Press done and you get the serial port back.
 *
 * 🚨 How recordings are stored does not change. The 24 MB partition has no
 * filesystem; audio is appended and that is the right shape for it. Putting a
 * real FAT there would throw that away and take every existing recording with
 * it. So this *pretends* to be FAT, and only while the host is reading: the
 * boot sector, the allocation table and the root directory are made up on the
 * spot, and file data is served straight out of the recording partition.
 *
 * 🚨 Read-only. Open it for writing and Windows starts trying to create
 * System Volume Information. Declare the medium write-protected and it
 * accepts that quietly, and we never write a single write path. Deleting
 * happens on the badge.
 *
 * A bonus falls out of making up the bytes: a **48-byte WAV header** can be
 * prepended. The files show up as REC0001.WAV and play on a double click.
 * IMA-ADPCM is a real WAV format (0x11), so nothing is converted.
 *
 * 🚨 On the S3, USB-Serial/JTAG and USB-OTG are separate peripherals sharing
 * the same pins. While TinyUSB holds them the serial port is gone. Even if
 * export mode crashes, the ROM bootloader always comes back as
 * USB-Serial/JTAG, so BOOT+RESET can always flash it — there is no way to
 * brick the board here.
 */
#include "usb_export.h"
#include "rec_export.h"
#include <string.h>
#include <stdio.h>

/* ── the shape of the volume ────────────────────────────────
 * FAT16, 512-byte sectors. Covering 24 MB wants large clusters, because that
 * is what keeps the allocation table small — at 32 KB per cluster, 24 MB is
 * 768 entries and the whole table still fits in one sector.
 * 🚨 A bigger table means either more RAM or more work per read. Large
 * clusters win. */
#define SEC          512u
#define CLUSTER_SEC  64u                    /* 32KB */
#define RESERVED     1u                     /* just the boot sector */
#define FAT_COPIES   1u
#define ROOT_ENTS    16u                    /* at most 12 recordings */
#define ROOT_SEC     ((ROOT_ENTS * 32u) / SEC)      /* = 1 */

/* 🚨 An IMA-ADPCM WAV header is 48 bytes, not 44. Its fmt chunk body is 20
 * bytes rather than 16 (samples-per-block is carried too), so everything
 * after it shifts. Assume 44 and the data size lands outside the header,
 * which makes the file unopenable rather than merely wrong. */
#define WAV_HDR      48u

/* Clusters in the volume: 24 MB / 32 KB = 768 */
#define DATA_CLUSTERS 768u
#define FAT_SEC       ((((DATA_CLUSTERS + 2u) * 2u) + SEC - 1u) / SEC)   /* = 3 */

#define LBA_FAT     (RESERVED)
#define LBA_ROOT    (LBA_FAT + FAT_SEC * FAT_COPIES)
#define LBA_DATA    (LBA_ROOT + ROOT_SEC)
#define TOTAL_SEC   (LBA_DATA + DATA_CLUSTERS * CLUSTER_SEC)

/* ── what gets exported ─────────────────────────────────────
 * One recording, one file. Clusters are handed out in order. */
typedef struct {
    char     name[12];        /* 8.3, space padded */
    uint32_t src_off;         /* data start, relative to the partition */
    uint32_t bytes;           /* ADPCM bytes */
    uint32_t first_clus;
    uint32_t clus_n;
} xfile_t;

static xfile_t s_f[REC_EXPORT_MAX];
static int     s_n;

static uint32_t file_total(const xfile_t *f) { return WAV_HDR + f->bytes; }

/* ── laying out the volume ──────────────────────────────────── */
void usb_export_build(void)
{
    rec_export_t list[REC_EXPORT_MAX];
    s_n = rec_export_list(list, REC_EXPORT_MAX);

    uint32_t clus = 2;                      /* 0 and 1 are reserved */
    for (int i = 0; i < s_n; i++) {
        snprintf(s_f[i].name, sizeof s_f[i].name, "REC%04dWAV", i + 1);
        s_f[i].src_off = list[i].off;
        s_f[i].bytes   = list[i].bytes;
        s_f[i].first_clus = clus;
        uint32_t tot = file_total(&s_f[i]);
        s_f[i].clus_n = (tot + (CLUSTER_SEC * SEC) - 1) / (CLUSTER_SEC * SEC);
        if (s_f[i].clus_n == 0) s_f[i].clus_n = 1;
        clus += s_f[i].clus_n;
        /* 🚨 Stop at the edge of the volume. Overrunning it makes the host
         * read the wrong place, which looks like a corrupt file rather than
         * like a full disk. */
        if (clus >= DATA_CLUSTERS + 2) { s_n = i + 1; break; }
    }
}

uint32_t usb_export_sectors(void) { return TOTAL_SEC; }
uint32_t usb_export_sector_size(void) { return SEC; }
int      usb_export_files(void) { return s_n; }

/* ── making up the pieces ───────────────────────────────────── */
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

static void boot_sector(uint8_t *b)
{
    memset(b, 0, SEC);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    put16(b + 11, SEC);
    b[13] = CLUSTER_SEC;
    put16(b + 14, RESERVED);
    b[16] = FAT_COPIES;
    put16(b + 17, ROOT_ENTS);
    put16(b + 19, TOTAL_SEC > 0xFFFF ? 0 : (uint16_t)TOTAL_SEC);
    b[21] = 0xF8;                            /* fixed disk */
    put16(b + 22, FAT_SEC);
    put16(b + 24, 1); put16(b + 26, 1);      /* anything will do */
    put32(b + 32, TOTAL_SEC > 0xFFFF ? TOTAL_SEC : 0);
    b[38] = 0x29;                            /* extended boot signature */
    put32(b + 39, 0x42414447);               /* volume serial */
    memcpy(b + 43, "BADGE REC  ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

static void fat_sector(uint32_t idx, uint8_t *b)
{
    memset(b, 0, SEC);
    /* The table holds 512/2 = 256 sixteen-bit entries per sector. */
    uint32_t base = idx * (SEC / 2);
    for (uint32_t k = 0; k < SEC / 2; k++) {
        uint32_t e = base + k;
        uint16_t v = 0;
        if (e == 0)      v = 0xFFF8;
        else if (e == 1) v = 0xFFFF;
        else {
    /* Which file is this entry, and how far into it? */
            for (int i = 0; i < s_n; i++) {
                if (e < s_f[i].first_clus || e >= s_f[i].first_clus + s_f[i].clus_n)
                    continue;
                v = (e + 1 == s_f[i].first_clus + s_f[i].clus_n) ? 0xFFFF
                                                                 : (uint16_t)(e + 1);
                break;
            }
        }
        put16(b + k * 2, v);
    }
}

static void root_sector(uint8_t *b)
{
    memset(b, 0, SEC);
    /* The first entry is the volume label */
    memcpy(b, "BADGE REC  ", 11);
    b[11] = 0x08;                            /* volume label */
    uint8_t *e = b + 32;
    for (int i = 0; i < s_n && (uint32_t)(i + 1) < ROOT_ENTS; i++, e += 32) {
        memcpy(e, s_f[i].name, 11);
        e[11] = 0x01;                        /* read only */
        /* No timestamps. Each recording has one, but getting it to agree with
         * FAT's local-time rules is work, and a blank field beats a wrong
         * date. */
        put16(e + 26, (uint16_t)s_f[i].first_clus);
        put32(e + 28, file_total(&s_f[i]));
    }
}

/* IMA-ADPCM WAV header. 🚨 Block alignment has to match what was stored. */
static void wav_header(const xfile_t *f, uint8_t *b)
{
    uint32_t data = f->bytes;
    uint32_t blk  = REC_ADPCM_BLOCK_BYTES;
    uint32_t spb  = REC_ADPCM_BLOCK_SAMPLES;
    memset(b, 0, WAV_HDR);
    memcpy(b, "RIFF", 4);
    put32(b + 4, 36 + data);
    memcpy(b + 8, "WAVEfmt ", 8);
    put32(b + 16, 20);                       /* fmt size (20 for ADPCM) */
    put16(b + 20, 0x0011);                   /* IMA ADPCM */
    put16(b + 22, 1);                        /* mono */
    put32(b + 24, REC_SAMPLE_RATE);
    put32(b + 28, REC_SAMPLE_RATE * blk / spb);
    put16(b + 32, (uint16_t)blk);
    put16(b + 34, 4);                        /* bits per sample */
    put16(b + 36, 2);                        /* extra size */
    put16(b + 38, (uint16_t)spb);
    memcpy(b + 40, "data", 4);
    put32(b + 44, data);
}

/* ── the host reads ─────────────────────────────────────────── */
bool usb_export_read(uint32_t lba, uint8_t *out)
{
    memset(out, 0, SEC);
    if (lba == 0) { boot_sector(out); return true; }
    if (lba < LBA_ROOT) { fat_sector(lba - LBA_FAT, out); return true; }
    if (lba < LBA_DATA) { root_sector(out); return true; }

    uint32_t rel = lba - LBA_DATA;
    uint32_t clus = rel / CLUSTER_SEC + 2;
    uint32_t in_clus = (rel % CLUSTER_SEC) * SEC;

    for (int i = 0; i < s_n; i++) {
        if (clus < s_f[i].first_clus || clus >= s_f[i].first_clus + s_f[i].clus_n)
            continue;
        uint32_t pos = (clus - s_f[i].first_clus) * (CLUSTER_SEC * SEC) + in_clus;
        uint32_t done = 0;
        /* First 48 bytes are the WAV header, the rest is the recording */
        if (pos < WAV_HDR) {
            uint8_t h[WAV_HDR];
            wav_header(&s_f[i], h);
            uint32_t n = WAV_HDR - pos;
            if (n > SEC) n = SEC;
            memcpy(out, h + pos, n);
            done = n;
        }
        uint32_t want = SEC - done;
        if (want) {
            uint32_t src = (pos + done) - WAV_HDR;
            if (src < s_f[i].bytes) {
                uint32_t n = s_f[i].bytes - src;
                if (n > want) n = want;
                rec_export_read(s_f[i].src_off + src, out + done, n);
            }
        }
        return true;
    }
    return true;                              /* past the end — hand back zeroes */
}
