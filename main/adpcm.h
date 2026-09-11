#pragma once
#include <stdint.h>
#include <stddef.h>

/* IMA ADPCM 인코더. 표준 WAV 블록 형식으로 낸다 —
 * 커스텀 포맷을 만들면 서버에서 디코더를 따로 붙여야 하는데,
 * 이 형식은 ffmpeg 도 Deepgram 도 그냥 읽는다.
 *
 * 블록 하나 = 256 바이트 = 헤더 4 + 코드 252 바이트
 *           = 첫 샘플 1개 + 504개 = 505 샘플
 * 16kHz 기준 초당 31.7 블록 = 8,110 바이트. 생 PCM 의 약 1/4. */
#define ADPCM_BLOCK_BYTES   256
#define ADPCM_BLOCK_SAMPLES 505

/* pcm 505개를 블록 하나(256B)로 만든다. 모자라면 0으로 채운다. */
void adpcm_encode_block(const int16_t *pcm, int n, uint8_t out[ADPCM_BLOCK_BYTES]);

/* 이 형식을 읽을 수 있게 WAV 머리를 만든다. 반환값 = 머리 길이. */
size_t adpcm_wav_header(uint8_t *out, uint32_t data_bytes, uint32_t sample_rate);
