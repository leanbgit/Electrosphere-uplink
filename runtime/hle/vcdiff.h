#ifndef AC3_VCDIFF_H
#define AC3_VCDIFF_H

#include "ac3_runtime.h"
#include <stddef.h>

typedef struct {
    u64 src_pos, src_len;
    u64 tpos;
    u32 tlen;
    int has_adler;
    u32 adler;
    const u8 *data, *inst, *addr;
    size_t data_len, inst_len, addr_len;
} vcd_window_t;

typedef struct {
    vcd_window_t *win;
    int nwin;
    u64 target_size;
    u32 max_tlen;
    char app[512];
    u8 *owned[3];
} vcd_patch_t;

typedef void (*vcd_copy_fn)(void *user, u64 target_off, u32 size, u64 source_off);

int  vcd_parse(const u8 *patch, size_t len, vcd_patch_t *out);
void vcd_free(vcd_patch_t *p);
int  vcd_apply_window(const vcd_window_t *w, const u8 *src, u8 *target, vcd_copy_fn fn, void *user);
u32  vcd_adler32(const u8 *p, size_t n);
u32  vcd_adler32_more(u32 sum, const u8 *p, size_t n);

#endif
