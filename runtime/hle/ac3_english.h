#ifndef AC3_ENGLISH_H
#define AC3_ENGLISH_H

#include "ac3_runtime.h"
#include <stddef.h>

typedef struct ac3_en_overlay ac3_en_overlay_t;

int  ac3_en_find_patch(int disc, const char *dir, char *out, size_t n);
ac3_en_overlay_t *ac3_en_overlay_build(const char *image_path, const char *patch_path);
ac3_en_overlay_t *ac3_en_overlay_open(const char *image_path, const char *patch_path, int *from_cache);
u32  ac3_en_overlay_sectors(const ac3_en_overlay_t *o);
const u8 *ac3_en_overlay_sector(const ac3_en_overlay_t *o, u32 lba, u32 *src_lba);
u32  ac3_en_overlay_literals(const ac3_en_overlay_t *o);
void ac3_en_overlay_free(ac3_en_overlay_t *o);

/* optional: called while ac3_en_overlay_build() works (the browser build shows
 * it as a loading bar); disc is ac3_en_progress_disc, set by the caller */
extern void (*ac3_en_progress)(int disc, u32 done, u32 total);
extern int ac3_en_progress_disc;

#endif
