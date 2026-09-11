/* 녹음을 밖으로 내보낼 때 쓰는 창구.
 *
 * 🚨 rec_store.c 의 안쪽 구조(디렉터리 섹터·순차 배치)를 밖으로 흘리지
 * 않으려고 이 창구만 연다. usb_export.c 는 "몇 개가 있고, 각각 어디서
 * 시작해 몇 바이트인가" 만 알면 된다. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REC_EXPORT_MAX 12

/* 저장 형식. WAV 머리를 지어낼 때 그대로 쓴다 —
 * 🚨 여기 값이 실제 저장과 어긋나면 파일이 열리긴 해도 소리가 깨진다. */
#define REC_SAMPLE_RATE         16000
#define REC_ADPCM_BLOCK_BYTES   256
#define REC_ADPCM_BLOCK_SAMPLES 505

typedef struct {
    uint32_t off;        /* 녹음 파티션 기준 데이터 시작 */
    uint32_t bytes;      /* ADPCM 바이트 */
    int64_t  started;    /* unix 초. 시계가 안 맞았으면 0 */
} rec_export_t;

/* 끝난 녹음만 준다(녹음 중인 것은 뺀다). 돌려주는 값은 개수. */
int  rec_export_list(rec_export_t *out, int max);
/* 파티션에서 그대로 퍼 온다. */
bool rec_export_read(uint32_t off, void *dst, uint32_t len);
