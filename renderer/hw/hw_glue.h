#ifndef AC3_HW_GLUE_H
#define AC3_HW_GLUE_H

#include "hw.h"
#include "hw_settings.h"

void hw_glue_attach(void);

void hw_glue_frame_end(const gpu_display_t *d);

int hw_glue_widescreen_active(void);

void hw_glue_toggle_fx(void);

void hw_rec_arm(void);

int hw_replay_main(const char *path, const char *out_prefix, const char *ram);

int ac3_selftest_hw(void);

#endif
