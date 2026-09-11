/* True LCD face — drawn with shapes, not bitmaps.
 *
 * The badge has no weather sensor, so the source design's temperature and
 * humidity rows are gone. What is left is what this board actually knows:
 * date, time, seconds, battery and uptime.
 *
 * Two modes share one implementation:
 *   active — grey-green TN panel, used by the Clock app
 *   aod    — black ground with dim segments, used by the lock screen
 *
 * Nothing here is baked into an image. The two dial bitmaps this replaces
 * cost 848 KB of flash between them; this costs a few hundred bytes of RAM.
 */
#ifndef LCDFACE_H
#define LCDFACE_H

#include "lvgl.h"
#include <stdbool.h>

typedef struct lcdface_s lcdface_t;

/* Builds the face into `parent` and fills it with the current time. */
lcdface_t *lcdface_create(lv_obj_t *parent, bool aod);

/* Call once a second. Only the digits that changed are touched. */
void lcdface_update(lcdface_t *f);

/* Frees the handle. The LVGL objects go with `parent`, so call this before
 * (or after) deleting the parent — never rely on it to delete them. */
void lcdface_destroy(lcdface_t *f);

#endif
