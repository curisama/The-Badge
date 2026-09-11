/* 녹음을 USB 드라이브로 내보낸다 — 읽기 전용 가짜 FAT.
 *
 * 꽂으면 COM(지금 그대로), "내보내기" 를 누르면 USB 드라이브, "끝" 이면 COM.
 *
 * 🚨 저장 방식은 안 건드린다. 녹음 칸(24MB)엔 파일시스템이 없고 순차로 붙여
 * 쓴다 — 그게 맞는 설계다(오디오는 append 뿐이다). FAT 으로 갈아엎으면 그
 * 이점을 버리고 기존 녹음도 날아간다. 대신 호스트가 읽는 순간에만 FAT 인
 * **척한다**: 부트섹터·FAT표·루트디렉터리를 그때그때 지어내고, 파일 데이터는
 * 녹음 파티션에서 그대로 퍼 온다.
 *
 * 🚨 읽기 전용이다. 쓰기를 열면 윈도우가 System Volume Information 을 만들려
 * 든다. "쓰기 금지" 매체로 알리면 순순히 받아들이고, 우리는 쓰기 처리를 아예
 * 안 만들어도 된다. 지우기는 배지에서 한다.
 *
 * 덤: 바이트를 우리가 지어내므로 파일 앞에 **WAV 머리 48바이트**를 얹는다.
 * REC0001.WAV 로 보이고 더블클릭하면 재생된다. IMA-ADPCM 은 WAV 의 정식
 * 포맷(0x11)이라 변환이 필요 없다.
 *
 * 🚨 S3 에서 USB-Serial/JTAG 와 USB-OTG 는 **같은 핀을 나눠 쓰는 별개
 * 주변장치**다. TinyUSB 가 핀을 가져가면 그 동안 COM 포트가 사라진다.
 * 내보내기 모드에서 뻗어도 ROM 부트로더는 항상 USB-Serial/JTAG 로 돌아오므로
 * BOOT+RESET 으로 구울 수 있다 — 벽돌 될 일은 없다.
 */
#include "usb_export.h"
#include "rec_export.h"
#include <string.h>
#include <stdio.h>

/* ── 판 모양 ────────────────────────────────────────────────
 * FAT16, 섹터 512B. 녹음 24MB 를 담으려면 클러스터를 크게 잡아야 FAT 표가
 * 작아진다 — 32KB 클러스터면 24MB 에 768칸, FAT 표가 한 섹터로도 남는다.
 * 🚨 표가 커지면 그만큼 램을 쓰거나 매번 지어내야 한다. 크게 잡는 게 이긴다. */
#define SEC          512u
#define CLUSTER_SEC  64u                    /* 32KB */
#define RESERVED     1u                     /* 부트섹터 하나 */
#define FAT_COPIES   1u
#define ROOT_ENTS    16u                    /* 녹음은 최대 12개 */
#define ROOT_SEC     ((ROOT_ENTS * 32u) / SEC)      /* = 1 */

/* 🚨 IMA-ADPCM WAV 머리는 44바이트가 아니라 **48바이트**다. fmt 본문이
 * 16이 아니라 20바이트라(블록당 표본 수가 더 붙는다) 그만큼 길어진다.
 * 44로 잡으면 data 크기를 머리 밖에 적게 되고 파일이 통째로 안 열린다. */
#define WAV_HDR      48u

/* 한 판에 담는 총 클러스터. 24MB / 32KB = 768 */
#define DATA_CLUSTERS 768u
#define FAT_SEC       ((((DATA_CLUSTERS + 2u) * 2u) + SEC - 1u) / SEC)   /* = 3 */

#define LBA_FAT     (RESERVED)
#define LBA_ROOT    (LBA_FAT + FAT_SEC * FAT_COPIES)
#define LBA_DATA    (LBA_ROOT + ROOT_SEC)
#define TOTAL_SEC   (LBA_DATA + DATA_CLUSTERS * CLUSTER_SEC)

/* ── 내보낼 파일 목록 ───────────────────────────────────────
 * 녹음 하나가 파일 하나. 클러스터는 앞에서부터 차례로 준다. */
typedef struct {
    char     name[12];        /* 8.3, 빈칸 채움 */
    uint32_t src_off;         /* 녹음 파티션 기준 데이터 시작 */
    uint32_t bytes;           /* ADPCM 바이트 */
    uint32_t first_clus;
    uint32_t clus_n;
} xfile_t;

static xfile_t s_f[REC_EXPORT_MAX];
static int     s_n;

static uint32_t file_total(const xfile_t *f) { return WAV_HDR + f->bytes; }

/* ── 판 짜기 ────────────────────────────────────────────────── */
void usb_export_build(void)
{
    rec_export_t list[REC_EXPORT_MAX];
    s_n = rec_export_list(list, REC_EXPORT_MAX);

    uint32_t clus = 2;                      /* 0·1 은 표에서 예약 */
    for (int i = 0; i < s_n; i++) {
        snprintf(s_f[i].name, sizeof s_f[i].name, "REC%04dWAV", i + 1);
        s_f[i].src_off = list[i].off;
        s_f[i].bytes   = list[i].bytes;
        s_f[i].first_clus = clus;
        uint32_t tot = file_total(&s_f[i]);
        s_f[i].clus_n = (tot + (CLUSTER_SEC * SEC) - 1) / (CLUSTER_SEC * SEC);
        if (s_f[i].clus_n == 0) s_f[i].clus_n = 1;
        clus += s_f[i].clus_n;
        /* 🚨 판을 넘치면 거기서 끊는다. 넘친 채로 내보내면 호스트가 엉뚱한
         * 자리를 읽어 파일이 깨진 것처럼 보인다. */
        if (clus >= DATA_CLUSTERS + 2) { s_n = i + 1; break; }
    }
}

uint32_t usb_export_sectors(void) { return TOTAL_SEC; }
uint32_t usb_export_sector_size(void) { return SEC; }
int      usb_export_files(void) { return s_n; }

/* ── 조각 만들기 ────────────────────────────────────────────── */
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
    b[21] = 0xF8;                            /* 고정 디스크 */
    put16(b + 22, FAT_SEC);
    put16(b + 24, 1); put16(b + 26, 1);      /* 아무 값 */
    put32(b + 32, TOTAL_SEC > 0xFFFF ? TOTAL_SEC : 0);
    b[38] = 0x29;                            /* 확장 부트 표시 */
    put32(b + 39, 0x42414447);               /* 일련번호 */
    memcpy(b + 43, "BADGE REC  ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

static void fat_sector(uint32_t idx, uint8_t *b)
{
    memset(b, 0, SEC);
    /* 표는 16비트 칸이 512/2 = 256개씩 들어간다. */
    uint32_t base = idx * (SEC / 2);
    for (uint32_t k = 0; k < SEC / 2; k++) {
        uint32_t e = base + k;
        uint16_t v = 0;
        if (e == 0)      v = 0xFFF8;
        else if (e == 1) v = 0xFFFF;
        else {
            /* 이 칸이 어느 파일의 몇 번째 클러스터인가 */
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
    /* 첫 칸은 판 이름 */
    memcpy(b, "BADGE REC  ", 11);
    b[11] = 0x08;                            /* 볼륨 라벨 */
    uint8_t *e = b + 32;
    for (int i = 0; i < s_n && (uint32_t)(i + 1) < ROOT_ENTS; i++, e += 32) {
        memcpy(e, s_f[i].name, 11);
        e[11] = 0x01;                        /* 읽기 전용 */
        /* 시각은 안 넣는다 — 녹음마다 들고 있지만 FAT 의 지역시각 규칙과
         * 맞추느라 틀린 값을 적느니 비워 두는 게 낫다. */
        put16(e + 26, (uint16_t)s_f[i].first_clus);
        put32(e + 28, file_total(&s_f[i]));
    }
}

/* IMA-ADPCM WAV 머리. 🚨 블록 정렬이 실제 저장과 같아야 재생된다. */
static void wav_header(const xfile_t *f, uint8_t *b)
{
    uint32_t data = f->bytes;
    uint32_t blk  = REC_ADPCM_BLOCK_BYTES;
    uint32_t spb  = REC_ADPCM_BLOCK_SAMPLES;
    memset(b, 0, WAV_HDR);
    memcpy(b, "RIFF", 4);
    put32(b + 4, 36 + data);
    memcpy(b + 8, "WAVEfmt ", 8);
    put32(b + 16, 20);                       /* fmt 크기 (ADPCM 은 20) */
    put16(b + 20, 0x0011);                   /* IMA ADPCM */
    put16(b + 22, 1);                        /* 모노 */
    put32(b + 24, REC_SAMPLE_RATE);
    put32(b + 28, REC_SAMPLE_RATE * blk / spb);
    put16(b + 32, (uint16_t)blk);
    put16(b + 34, 4);                        /* 표본당 비트 */
    put16(b + 36, 2);                        /* 덧붙임 크기 */
    put16(b + 38, (uint16_t)spb);
    memcpy(b + 40, "data", 4);
    put32(b + 44, data);
}

/* ── 호스트가 읽어 간다 ─────────────────────────────────────── */
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
        /* 앞 48바이트는 WAV 머리, 그 뒤는 녹음 원본 */
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
    return true;                              /* 빈 자리 — 0 으로 채워 준다 */
}
