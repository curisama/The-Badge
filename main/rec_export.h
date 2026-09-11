/* The window used for taking recordings off the badge.
 *
 * 🚨 Only this window is opened, to keep rec_store.c's internals (the
 * directory sector, the sequential layout) from leaking out. usb_export.c only
 * needs to know how many there are and where each one starts and how long it
 * is. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REC_EXPORT_MAX 12

/* The stored format. Used as-is when making up the WAV header —
 * 🚨 if these values disagree with what is actually stored, the file opens but
 * the sound is broken. */
#define REC_SAMPLE_RATE         16000
#define REC_ADPCM_BLOCK_BYTES   256
#define REC_ADPCM_BLOCK_SAMPLES 505

typedef struct {
    uint32_t off;        /* data start, relative to the recording partition */
    uint32_t bytes;      /* ADPCM bytes */
    int64_t  started;    /* unix seconds; 0 if the clock was not set */
} rec_export_t;

/* Only finished recordings (one in progress is left out). Returns the count. */
int  rec_export_list(rec_export_t *out, int max);
/* Read straight out of the partition. */
bool rec_export_read(uint32_t off, void *dst, uint32_t len);
