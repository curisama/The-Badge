#pragma once
#include <stdint.h>
#include <stddef.h>

/* An IMA ADPCM encoder, producing standard WAV blocks —
 * a custom format would mean bolting a decoder onto the server, whereas both
 * ffmpeg and Deepgram read this one directly.
 *
 * One block = 256 bytes = 4 header + 252 code bytes
 *           = 1 first sample + 504 = 505 samples
 * At 16 kHz that is 31.7 blocks or 8,110 bytes a second, about a quarter of raw PCM. */
#define ADPCM_BLOCK_BYTES   256
#define ADPCM_BLOCK_SAMPLES 505

/* Turns 505 pcm samples into one block (256 B). Short input is padded with zeros. */
void adpcm_encode_block(const int16_t *pcm, int n, uint8_t out[ADPCM_BLOCK_BYTES]);

/* Builds a WAV header so the format can be read. Returns the header length. */
size_t adpcm_wav_header(uint8_t *out, uint32_t data_bytes, uint32_t sample_rate);
