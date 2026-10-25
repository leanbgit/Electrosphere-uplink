#include "ac3_runtime.h"
#include <stdio.h>

static void once(const char *what, u32 detail) {
    enum { CAP = 256 };
    static const char *seen_what[CAP];
    static u32 seen_detail[CAP];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (seen_what[i] == what && seen_detail[i] == detail) return;
    if (n < CAP) {
        seen_what[n] = what;
        seen_detail[n] = detail;
        n++;
    }
    fprintf(stderr, "ac3: STUB %s (0x%X)\n", what, detail);
}

void ac3_mtc0(ctx_t *ctx, u32 reg, u32 value) {
    if (reg < 16) ctx->cop0[reg] = value;
}

u32 ac3_mfc0(ctx_t *ctx, u32 reg) {
    return (reg < 16) ? ctx->cop0[reg] : 0u;
}

void ac3_rfe(ctx_t *ctx) {
    u32 sr = ctx->cop0[12];
    ctx->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
}
