#include "adpcm.h"
#include <string.h>

static const int16_t STEP[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
    80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
    8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767,
};
static const int8_t INDEX[16] = {
    -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8,
};

/* 인코더 상태는 블록 사이에 이어진다. 블록 머리에 예측값과 인덱스를
 * 같이 적어두므로 디코더가 블록 하나만으로도 따라올 수 있다. */
static int32_t s_pred;
static int8_t  s_index;

static uint8_t encode_one(int16_t sample)
{
    int step = STEP[s_index];
    int diff = sample - s_pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }

    int delta = step >> 3;
    if (diff >= step)      { code |= 4; diff -= step;      delta += step; }
    step >>= 1;
    if (diff >= step)      { code |= 2; diff -= step;      delta += step; }
    step >>= 1;
    if (diff >= step)      { code |= 1;                    delta += step; }

    s_pred += (code & 8) ? -delta : delta;
    if (s_pred >  32767) s_pred =  32767;
    if (s_pred < -32768) s_pred = -32768;

    s_index += INDEX[code];
    if (s_index < 0)  s_index = 0;
    if (s_index > 88) s_index = 88;
    return code;
}

void adpcm_encode_block(const int16_t *pcm, int n, uint8_t out[ADPCM_BLOCK_BYTES])
{
    memset(out, 0, ADPCM_BLOCK_BYTES);
    if (n <= 0) return;

    /* 블록 머리 — 첫 샘플을 그대로 싣고 거기서 다시 시작한다.
     * 이래야 앞 블록이 깨져도 뒤가 안 무너진다. */
    s_pred = pcm[0];
    out[0] = (uint8_t)(s_pred & 0xFF);
    out[1] = (uint8_t)((s_pred >> 8) & 0xFF);
    out[2] = (uint8_t)s_index;
    out[3] = 0;

    for (int i = 1; i < ADPCM_BLOCK_SAMPLES; i++) {
        uint8_t c = encode_one(i < n ? pcm[i] : (int16_t)s_pred);
        int pos = i - 1;                     /* 코드 순서: 낮은 니블 먼저 */
        if (pos & 1) out[4 + pos / 2] |= (uint8_t)(c << 4);
        else         out[4 + pos / 2]  = c;
    }
}

static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

size_t adpcm_wav_header(uint8_t *out, uint32_t data_bytes, uint32_t sample_rate)
{
    uint32_t blocks  = data_bytes / ADPCM_BLOCK_BYTES;
    uint32_t samples = blocks * ADPCM_BLOCK_SAMPLES;
    uint32_t bps     = (sample_rate / ADPCM_BLOCK_SAMPLES) * ADPCM_BLOCK_BYTES;

    memcpy(out, "RIFF", 4);
    put32(out + 4, 4 + 8 + 20 + 8 + 4 + 8 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 20);                       /* fmt 길이 */
    put16(out + 20, 0x0011);                   /* IMA ADPCM */
    put16(out + 22, 1);                        /* 모노 */
    put32(out + 24, sample_rate);
    put32(out + 28, bps);
    put16(out + 32, ADPCM_BLOCK_BYTES);        /* 블록 정렬 */
    put16(out + 34, 4);                        /* 샘플당 비트 */
    put16(out + 36, 2);                        /* cbSize */
    put16(out + 38, ADPCM_BLOCK_SAMPLES);
    memcpy(out + 40, "fact", 4);               /* 압축 형식은 fact 가 있어야 안전하다 */
    put32(out + 44, 4);
    put32(out + 48, samples);
    memcpy(out + 52, "data", 4);
    put32(out + 56, data_bytes);
    return 60;
}
