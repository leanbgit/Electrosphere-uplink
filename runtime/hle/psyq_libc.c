#include "hle.h"
#include "ac3_oracle.h"
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

static u32 varg(ctx_t *ctx, int skip, int n) {
    int idx = skip + n;
    if (idx < 4) return ctx->r[4 + idx];
    return MEM_R32(ctx->r[29] + 16u + 4u * (u32)(idx - 4));
}

static void guest_str(u32 addr, char *out, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        u8 c = (u8)MEM_R8U(addr + (u32)i);
        if (!c) break;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

typedef void (*emit_fn)(void *user, char c);

static void fmt_engine(ctx_t *ctx, int skip, u32 fmt_addr,
                       emit_fn emit, void *user) {
    char spec[32], tmp[128];
    u32 p = fmt_addr;
    int argn = 0;

    for (;;) {
        u8 c = (u8)MEM_R8U(p++);
        if (!c) break;
        if (c != '%') { emit(user, (char)c); continue; }

        int si = 0;
        spec[si++] = '%';
        u8 conv = 0;
        while (si < (int)sizeof spec - 1) {
            u8 d = (u8)MEM_R8U(p++);
            if (!d) { conv = 0; break; }
            spec[si++] = (char)d;
            if (strchr("diouxXcspfgeEG%", (int)d)) { conv = d; break; }
        }
        spec[si] = 0;
        if (!conv) break;

        switch (conv) {
        case '%':
            emit(user, '%');
            break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
            u32 v = varg(ctx, skip, argn++);
            char hostspec[34];
            int n = 0;
            for (int k = 0; k < si; k++) {
                if (spec[k] == 'l') continue;
                if (k == si - 1) hostspec[n++] = 'l';
                hostspec[n++] = spec[k];
            }
            hostspec[n] = 0;
            snprintf(tmp, sizeof tmp, hostspec,
                     (conv == 'd' || conv == 'i') ? (long)(s32)v : (long)(u32)v);
            for (char *q = tmp; *q; q++) emit(user, *q);
            break;
        }
        case 'c':
            emit(user, (char)varg(ctx, skip, argn++));
            break;
        case 's': {
            u32 sa = varg(ctx, skip, argn++);
            char sbuf[512];
            guest_str(sa, sbuf, sizeof sbuf);
            snprintf(tmp, sizeof tmp, spec, sbuf);
            for (char *q = tmp; *q; q++) emit(user, *q);
            break;
        }
        case 'p': {
            u32 v = varg(ctx, skip, argn++);
            snprintf(tmp, sizeof tmp, "%08lx", (unsigned long)v);
            for (char *q = tmp; *q; q++) emit(user, *q);
            break;
        }
        default:
            hle_warn("printf", "unsupported conversion %%%c", conv);
            argn++;
            break;
        }
    }
}

typedef struct { int n; } stdout_sink;
static void emit_stdout(void *u, char c) { ((stdout_sink *)u)->n++; fputc(c, stderr); }

typedef struct { u32 addr; int n; } guest_sink;
static void emit_guest(void *u, char c) {
    guest_sink *g = (guest_sink *)u;
    MEM_W8(g->addr + (u32)g->n, (u8)c);
    g->n++;
}

void psyq_printf(ctx_t *ctx) {
    stdout_sink s = { 0 };
    fmt_engine(ctx, 1, A0, emit_stdout, &s);
    RET(s.n);
}

void psyq_prnt(ctx_t *ctx) { psyq_printf(ctx); }

void psyq_sprintf(ctx_t *ctx) {
    guest_sink g = { A0, 0 };
    fmt_engine(ctx, 2, A1, emit_guest, &g);
    MEM_W8(g.addr + (u32)g.n, 0);
    RET(g.n);
}

void psyq_puts(ctx_t *ctx) {
    char buf[1024];
    guest_str(A0, buf, sizeof buf);
    fprintf(stderr, "%s\n", buf);
    RET(0);
}

void psyq_putchar(ctx_t *ctx)        { fputc((int)(A0 & 0xFF), stderr); RET(A0); }
void psyq__putchar(ctx_t *ctx)       { psyq_putchar(ctx); }
void psyq__putchar_flash(ctx_t *ctx) { psyq_putchar(ctx); }

void psyq_memcpy(ctx_t *ctx) {
    u32 d = A0, s = A1, n = A2;
    void *dp = guest_ptr(d, n), *sp = guest_ptr(s, n);
    if (dp && sp) memcpy(dp, sp, n);
    else for (u32 i = 0; i < n; i++) MEM_W8(d + i, MEM_R8U(s + i));
    ac3_watch_bulk(d, n, "memcpy");
    RET(d);
}

void psyq_memmove(ctx_t *ctx) {
    u32 d = A0, s = A1, n = A2;
    void *dp = guest_ptr(d, n), *sp = guest_ptr(s, n);
    if (dp && sp) memmove(dp, sp, n);
    else if (d < s) for (u32 i = 0; i < n; i++) MEM_W8(d + i, MEM_R8U(s + i));
    else for (u32 i = n; i-- > 0;) MEM_W8(d + i, MEM_R8U(s + i));
    RET(d);
}

void psyq_memset(ctx_t *ctx) {
    u32 d = A0, c = A1 & 0xFF, n = A2;
    void *dp = guest_ptr(d, n);
    if (dp) memset(dp, (int)c, n);
    else for (u32 i = 0; i < n; i++) MEM_W8(d + i, c);
    RET(d);
}

void psyq_bzero(ctx_t *ctx) {
    u32 d = A0, n = A1;
    void *dp = guest_ptr(d, n);
    if (dp) memset(dp, 0, n);
    else for (u32 i = 0; i < n; i++) MEM_W8(d + i, 0);
    RET(d);
}

void psyq_memchr(ctx_t *ctx) {
    u32 p = A0, c = A1 & 0xFF, n = A2;
    for (u32 i = 0; i < n; i++)
        if (MEM_R8U(p + i) == c) { RET(p + i); return; }
    RET(0);
}

void psyq_strlen(ctx_t *ctx) {
    u32 p = A0, n = 0;
    while (MEM_R8U(p + n)) n++;
    RET(n);
}

void psyq_strcmp(ctx_t *ctx) {
    u32 a = A0, b = A1;
    for (;;) {
        u32 ca = MEM_R8U(a++), cb = MEM_R8U(b++);
        if (ca != cb) { RET((u32)((s32)ca - (s32)cb)); return; }
        if (!ca) { RET(0); return; }
    }
}

void psyq_strncmp(ctx_t *ctx) {
    u32 a = A0, b = A1, n = A2;
    for (u32 i = 0; i < n; i++) {
        u32 ca = MEM_R8U(a + i), cb = MEM_R8U(b + i);
        if (ca != cb) { RET((u32)((s32)ca - (s32)cb)); return; }
        if (!ca) break;
    }
    RET(0);
}

void psyq_strcpy(ctx_t *ctx) {
    u32 d = A0, s = A1, i = 0;
    for (;;) {
        u32 c = MEM_R8U(s + i);
        MEM_W8(d + i, c);
        if (!c) break;
        i++;
    }
    RET(d);
}

void psyq_strcat(ctx_t *ctx) {
    u32 d = A0, s = A1, n = 0, i = 0;
    while (MEM_R8U(d + n)) n++;
    for (;;) {
        u32 c = MEM_R8U(s + i);
        MEM_W8(d + n + i, c);
        if (!c) break;
        i++;
    }
    RET(d);
}

#define RAND_SEED_ADDR 0x800CBF58u

void psyq_rand(ctx_t *ctx) {
    if (ac3_oracle_in_draw()) ac3_oracle_rand_caller(ctx->r[31]);
    u32 seed = MEM_R32(RAND_SEED_ADDR) * 1103515245u + 12345u;
    MEM_W32(RAND_SEED_ADDR, seed);
    RET((seed >> 16) & 0x7FFFu);
}

void psyq_srand(ctx_t *ctx) { MEM_W32(RAND_SEED_ADDR, A0); RET(0); }

#define JB_PC 0
#define JB_SP 1
#define JB_FP 2
#define JB_S0 3
#define JB_GP 11
#define JB_WORDS 12

#define MAX_JMP 8
static struct { jmp_buf env; u32 guest_buf; int active; } jmp_slots[MAX_JMP];

static void jb_save(ctx_t *ctx, u32 buf) {
    MEM_W32(buf + 4u * JB_PC, ctx->r[31]);
    MEM_W32(buf + 4u * JB_SP, ctx->r[29]);
    MEM_W32(buf + 4u * JB_FP, ctx->r[30]);
    for (int i = 0; i < 8; i++)
        MEM_W32(buf + 4u * (u32)(JB_S0 + i), ctx->r[16 + i]);
    MEM_W32(buf + 4u * JB_GP, ctx->r[28]);
}

static void jb_restore(ctx_t *ctx, u32 buf) {
    ctx->r[31] = MEM_R32(buf + 4u * JB_PC);
    ctx->r[29] = MEM_R32(buf + 4u * JB_SP);
    ctx->r[30] = MEM_R32(buf + 4u * JB_FP);
    for (int i = 0; i < 8; i++)
        ctx->r[16 + i] = MEM_R32(buf + 4u * (u32)(JB_S0 + i));
    ctx->r[28] = MEM_R32(buf + 4u * JB_GP);
}

int ac3_setjmp_save(ctx_t *ctx, u32 buf) {
    jb_save(ctx, buf);
    int slot = -1;
    for (int i = 0; i < MAX_JMP; i++)
        if (jmp_slots[i].active && jmp_slots[i].guest_buf == buf) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < MAX_JMP; i++)
            if (!jmp_slots[i].active) { slot = i; break; }
    if (slot < 0) {
        hle_warn("setjmp", "no free slot (%d nested)", MAX_JMP);
        return 0;
    }
    jmp_slots[slot].active = 1;
    jmp_slots[slot].guest_buf = buf;
    return slot;
}

jmp_buf *ac3_setjmp_env(int slot) {
    return &jmp_slots[slot < 0 || slot >= MAX_JMP ? 0 : slot].env;
}

void psyq_setjmp(ctx_t *ctx) {
    hle_warn("setjmp",
             "called indirectly; cannot capture the caller's frame. "
             "The jump target will be wrong if longjmp is used.");
    ac3_setjmp_save(ctx, A0);
    RET(0);
}

void psyq_longjmp(ctx_t *ctx) {
    u32 buf = A0;
    s32 val = (s32)A1;
    if (val == 0) val = 1;

    for (int i = 0; i < MAX_JMP; i++) {
        if (jmp_slots[i].active && jmp_slots[i].guest_buf == buf) {
            jb_restore(ctx, buf);
            ctx->r[2] = (u32)val;
            longjmp(jmp_slots[i].env, val);
        }
    }
    hle_warn("longjmp", "no matching setjmp for buf 0x%08X", buf);
    RET(0);
}
