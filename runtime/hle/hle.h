#ifndef AC3_HLE_H
#define AC3_HLE_H

#include "ac3_runtime.h"
#include <stdio.h>

#define A0 (ctx->r[4])
#define A1 (ctx->r[5])
#define A2 (ctx->r[6])
#define A3 (ctx->r[7])
#define ASTACK(n) MEM_R32(ctx->r[29] + 4u * (n))
#define RET(v) (ctx->r[2] = (u32)(v))

static inline void *guest_ptr(u32 addr, u32 size) {
    u32 p = ac3_phys(addr);
    if (p >= AC3_RAM_SIZE || size > AC3_RAM_SIZE - p) return NULL;
    return ac3_ram + p;
}

#define GUEST(addr, type) ((type *)guest_ptr((addr), sizeof(type)))

void hle_warn(const char *fn, const char *fmt, ...);
void hle_trace(const char *fn, const char *fmt, ...);
void hle_warn_always(const char *fn, const char *fmt, ...);
void ac3_watch_bulk(u32 addr, u32 len, const char *who);
extern int ac3_hle_trace_enabled;

typedef struct { s16 x, y, w, h; } PSX_RECT;

typedef struct {
    u32 tag;
    u32 code[15];
} PSX_DR_ENV;

typedef struct {
    PSX_RECT clip;
    s16 ofs[2];
    PSX_RECT tw;
    u16 tpage;
    u8 dtd;
    u8 dfe;
    u8 isbg;
    u8 r0, g0, b0;
    PSX_DR_ENV dr_env;
} PSX_DRAWENV;

typedef struct {
    PSX_RECT disp;
    PSX_RECT screen;
    u8 isinter;
    u8 isrgb24;
    u8 pad0, pad1;
} PSX_DISPENV;

typedef struct {
    u32 tag;
    u8 r0, g0, b0, code;
} PSX_P_TAG;

#define OT_ADDR(tag) ((tag) & 0x00FFFFFFu)
#define OT_LEN(tag)  (((tag) >> 24) & 0xFFu)
#define OT_TERM      0x00FFFFFFu

#endif
