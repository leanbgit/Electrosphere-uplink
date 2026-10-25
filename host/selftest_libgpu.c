#include "ac3_runtime.h"
#include "gpu_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ac3_mem_init(void);

#define G_STACK   0x8015F000u
#define G_ARG0    0x80150000u
#define G_ARG1    0x80151000u
#define G_OT      0x80152000u
#define G_PRIM    0x80153000u

#define G_DRIVER_PTR   0x800BB95Cu
#define G_DRIVER_TABLE 0x800BB91Cu

#define G_LIMIT_W 0x800BB968u
#define G_LIMIT_H 0x800BB96Au

static int load_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    u8 head[0x800];
    if (fread(head, 1, sizeof head, f) != sizeof head ||
        memcmp(head, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s is not a PS-X EXE\n", path);
        fclose(f); return 0;
    }
    u32 dest, size;
    memcpy(&dest, head + 0x18, 4);
    memcpy(&size, head + 0x1C, 4);
    u32 phys = dest & 0x1FFFFFu;
    if (phys + size > AC3_RAM_SIZE) { fclose(f); return 0; }
    if (fread(ac3_ram + phys, 1, size, f) != size) { fclose(f); return 0; }
    fclose(f);
    printf("loaded %s  (0x%08X, %u bytes)\n\n", path, dest, size);
    return 1;
}

typedef void (*setup_fn)(ctx_t *c);

static void put_rect(u32 at, int x, int y, int w, int h) {
    MEM_W16(at + 0, (u16)x); MEM_W16(at + 2, (u16)y);
    MEM_W16(at + 4, (u16)w); MEM_W16(at + 6, (u16)h);
}

static void arg5(ctx_t *c, u32 v) { MEM_W32(c->r[29] + 16, v); }

static void s_defdrawenv(ctx_t *c) {
    c->r[4] = G_ARG0; c->r[5] = 0; c->r[6] = 0; c->r[7] = 320; arg5(c, 240);
}
static void s_defdispenv(ctx_t *c) {
    c->r[4] = G_ARG0; c->r[5] = 0; c->r[6] = 0; c->r[7] = 320; arg5(c, 240);
}
static void s_drawmode(ctx_t *c) {
    put_rect(G_ARG1, 0, 0, 0, 0);
    c->r[4] = G_ARG0; c->r[5] = 1; c->r[6] = 1; c->r[7] = 0x1F; arg5(c, G_ARG1);
}
static void s_drawtpage(ctx_t *c) {
    c->r[4] = G_ARG0; c->r[5] = 1; c->r[6] = 1; c->r[7] = 0x1F;
}
static void s_drawarea(ctx_t *c) {
    put_rect(G_ARG1, 12, 20, 64, 58);
    c->r[4] = G_ARG0; c->r[5] = G_ARG1;
}
static void s_drawarea_backbuf(ctx_t *c) {
    put_rect(G_ARG1, 0, 240, 512, 240);
    c->r[4] = G_ARG0; c->r[5] = G_ARG1;
}
static void s_drawoffset(ctx_t *c) {
    MEM_W16(G_ARG1 + 0, 160); MEM_W16(G_ARG1 + 2, 120);
    c->r[4] = G_ARG0; c->r[5] = G_ARG1;
}
static void s_drawload(ctx_t *c) {
    put_rect(G_ARG1, 320, 256, 16, 1);
    c->r[4] = G_ARG0; c->r[5] = G_ARG1;
}
static void s_catprim(ctx_t *c) {
    MEM_W32(G_PRIM,        0x0500ABCDu);
    MEM_W32(G_PRIM + 0x40, 0x0300FFFFu);
    c->r[4] = G_PRIM; c->r[5] = G_PRIM + 0x40;
}
static void s_clearot(ctx_t *c) { c->r[4] = G_OT; c->r[5] = 16; }
static void s_gettpage(ctx_t *c) {
    c->r[4] = 0; c->r[5] = 1; c->r[6] = 384; c->r[7] = 256;
}
static void s_getclut(ctx_t *c) { c->r[4] = 384; c->r[5] = 510; }

static void s_gettpage_lowy(ctx_t *c) {
    c->r[4] = 1; c->r[5] = 2; c->r[6] = 640; c->r[7] = 264;
}
static void s_gettpage_highy(ctx_t *c) {
    c->r[4] = 2; c->r[5] = 3; c->r[6] = 960; c->r[7] = 512;
}
static void s_getclut_highy(ctx_t *c) { c->r[4] = 640; c->r[5] = 520; }
static void s_drawload_toobig(ctx_t *c) {
    put_rect(G_ARG1, 320, 256, 16, 4);
    c->r[4] = G_ARG0; c->r[5] = G_ARG1;
}

static void s_loadimage(ctx_t *c) {
    put_rect(G_ARG1, 320, 256, 16, 1);
    for (int i = 0; i < 16; i++) MEM_W32(G_PRIM + 4u * (u32)i, 0x1000u + i);
    c->r[4] = G_ARG1; c->r[5] = G_PRIM;
}
static void s_storeimage(ctx_t *c) {
    put_rect(G_ARG1, 320, 256, 16, 1);
    c->r[4] = G_ARG1; c->r[5] = G_PRIM;
}
static void s_moveimage(ctx_t *c) {
    put_rect(G_ARG1, 320, 256, 16, 1);
    c->r[4] = G_ARG1; c->r[5] = 640; c->r[6] = 300;
}
static void s_putdrawenv(ctx_t *c) {
    put_rect(G_ARG0 + 0, 0, 0, 320, 240);
    MEM_W16(G_ARG0 + 8, 0); MEM_W16(G_ARG0 + 10, 0);
    put_rect(G_ARG0 + 12, 0, 0, 0, 0);
    MEM_W16(G_ARG0 + 20, 0x000A);
    MEM_W8(G_ARG0 + 22, 1);
    MEM_W8(G_ARG0 + 23, 0);
    MEM_W8(G_ARG0 + 24, 0);
    c->r[4] = G_ARG0;
}

static const struct {
    const char *name; u32 addr; setup_fn setup; int ret; int hw;
    u32 skip_lo, skip_hi;
} cases[] = {
    { "SetDefDrawEnv", 0x800AC5C8u, s_defdrawenv,  0,  0,  0, 0 },
    { "SetDefDispEnv", 0x800AC688u, s_defdispenv,  0,  0,  0, 0 },
    { "SetDrawMode",   0x800AC7F8u, s_drawmode,    0,  0,  0, 0 },
    { "SetDrawTPage",  0x800AC758u, s_drawtpage,   0,  0,  0, 0 },
    { "SetDrawArea",   0x800A8F20u, s_drawarea,    0,  0,  0, 0 },
    { "SetDrawArea bb",0x800A8F20u, s_drawarea_backbuf, 0, 0, 0, 0 },
    { "SetDrawOffset", 0x800A8FA0u, s_drawoffset,  0,  0,  0, 0 },
    { "SetDrawLoad",   0x800AC788u, s_drawload,    0,  0,  0, 0 },
    { "CatPrim",       0x800AC728u, s_catprim,     0,  0,  0, 0 },
    { "ClearOTag",     0x800A85B8u, s_clearot,     0,  0,  0, 0 },
    { "ClearOTagR",    0x800A8680u, s_clearot,     0,  0,
      G_OT, G_OT + 4u * 16u },
    { "GetTPage",      0x800AC6C8u, s_gettpage,    1,  0,  0, 0 },
    { "GetClut",       0x800AC708u, s_getclut,     1,  0,  0, 0 },
    { "GetTPage y+8",  0x800AC6C8u, s_gettpage_lowy,  1, 0, 0, 0 },
    { "GetTPage y512", 0x800AC6C8u, s_gettpage_highy, 1, 0, 0, 0 },
    { "GetClut y520",  0x800AC708u, s_getclut_highy,  1, 0, 0, 0 },
    { "SetDrawLoad 2big", 0x800AC788u, s_drawload_toobig, 0, 0, 0, 0 },
    { "PutDrawEnv",    0x800A87F8u, s_putdrawenv,  0,  1,
      G_ARG0 + 28u, G_ARG0 + 92u },
    { "LoadImage",     0x800A8440u, s_loadimage,   0,  1,  0, 0 },
    { "StoreImage",    0x800A84A0u, s_storeimage,  0,  1,
      G_PRIM, G_PRIM + 0x40u },
    { "MoveImage",     0x800A8500u, s_moveimage,   0,  1,  0, 0 },
};

#define MMIO_ARTEFACT_LIMIT 0x1000u

#define STACK_LO  0x8015E000u
#define STACK_HI  0x8015F000u

static void prepare(ctx_t *c) {
    memset(c, 0, sizeof *c);
    c->r[28] = 0x800BCB74u;
    c->r[29] = G_STACK;
    memset(ac3_ram + (G_ARG0 & 0x1FFFFFu), 0, 0x6000);
}

static int compare(const char *name, const u8 *a, const u8 *b,
                   u32 ra, u32 rb, int cmp_ret, int hw,
                   u32 skip_lo, u32 skip_hi) {
    u32 diffs = 0, artefacts = 0, first = 0, last = 0;
    for (u32 i = 0; i < AC3_RAM_SIZE; i++) {
        if (a[i] == b[i]) continue;
        if (i < MMIO_ARTEFACT_LIMIT) { artefacts++; continue; }
        u32 ga = 0x80000000u + i;
        if (ga >= STACK_LO && ga < STACK_HI) continue;
        if (skip_hi && ga >= skip_lo && ga < skip_hi) continue;
        if (ga == 0x800BBAA0u) continue;
        if (!diffs) first = i;
        last = i;
        diffs++;
    }
    int ok = (diffs == 0) && (!cmp_ret || ra == rb);
    if (hw) {
        printf("  %-14s %s\n", name,
               diffs ? "STRAY GUEST WRITES"
                     : "effect is in hardware, no RAM to compare");
        return diffs == 0;
    }
    printf("  %-14s %s", name, ok ? "match" : "DIFFERS");
    if (cmp_ret && ra != rb) printf("   return 0x%08X vs 0x%08X", ra, rb);
    if (diffs) printf("   %u bytes differ, 0x%08X..0x%08X",
                      diffs, 0x80000000u + first, 0x80000000u + last);
    if (artefacts) printf("   (+%u bytes of hardware-register artefact)",
                          artefacts);
    printf("\n");

    if (diffs) {
        u32 shown = 0;
        for (u32 i = first & ~3u; i <= last && shown < 6; i += 4) {
            u32 wa, wb;
            memcpy(&wa, a + i, 4); memcpy(&wb, b + i, 4);
            if (wa == wb) continue;
            printf("      0x%08X : real 0x%08X   hle 0x%08X\n",
                   0x80000000u + i, wa, wb);
            shown++;
        }
    }
    return ok;
}

extern u32 ac3_gpu_build_dr_env(u32 env_addr);
#define G_SETDRAWENV 0x800A91D8u

typedef struct {
    const char *name;
    s16 clip[4], ofs[2], tw[4];
    u16 tpage; u8 dtd, dfe, isbg, r, g, b;
} drenv_case_t;

static const drenv_case_t drenv_cases[] = {
    { "DR_ENV plain",    { 0, 0, 320, 240 },   { 0, 0 },    { 0, 0, 0, 0 },
      0x000A, 1, 1, 0, 0, 0, 0 },
    { "DR_ENV fill",     { 0, 256, 320, 240 }, { 0, 256 },  { 0, 0, 0, 0 },
      0x000A, 1, 1, 1, 0, 112, 120 },
    { "DR_ENV tile",     { 8, 16, 300, 200 },  { 20, -4 },  { 0, 0, 0, 0 },
      0x0025, 0, 1, 1, 1, 2, 3 },
    { "DR_ENV clamp+tw", { 100, 50, 2000, 700 }, { -7, 9 }, { 16, 32, 32, 64 },
      0x019F, 0, 1, 1, 200, 100, 50 },
};

static int check_dr_env(void) {
    int ok = 1;
    for (unsigned i = 0; i < sizeof drenv_cases / sizeof drenv_cases[0]; i++) {
        const drenv_case_t *k = &drenv_cases[i];
        u32 words[2][10];
        for (int run = 0; run < 2; run++) {
            ctx_t c;
            prepare(&c);
            put_rect(G_ARG0 + 0, k->clip[0], k->clip[1], k->clip[2], k->clip[3]);
            MEM_W16(G_ARG0 + 8, (u16)k->ofs[0]);
            MEM_W16(G_ARG0 + 10, (u16)k->ofs[1]);
            put_rect(G_ARG0 + 12, k->tw[0], k->tw[1], k->tw[2], k->tw[3]);
            MEM_W16(G_ARG0 + 20, k->tpage);
            MEM_W8(G_ARG0 + 22, k->dtd);
            MEM_W8(G_ARG0 + 23, k->dfe);
            MEM_W8(G_ARG0 + 24, k->isbg);
            MEM_W8(G_ARG0 + 25, k->r);
            MEM_W8(G_ARG0 + 26, k->g);
            MEM_W8(G_ARG0 + 27, k->b);
            MEM_W32(G_ARG0 + 28, 0x00ABCDEFu);
            if (run == 0) {
                c.r[4] = G_ARG0 + 28u;
                c.r[5] = G_ARG0;
                ac3_interp_step_budget = 8000000u;
                ac3_interpret(&c, G_SETDRAWENV);
                ac3_interp_step_budget = 0;
            } else {
                ac3_gpu_build_dr_env(G_ARG0);
            }
            for (int w = 0; w < 10; w++)
                words[run][w] = MEM_R32(G_ARG0 + 28u + 4u * (u32)w);
        }
        u32 len = words[0][0] >> 24;
        int same = (words[0][0] == words[1][0]);
        for (u32 w = 1; w <= len && w < 10; w++) same &= words[0][w] == words[1][w];
        printf("  %-14s %s  (%u words)\n", k->name, same ? "match" : "DIFFERS", len);
        if (!same)
            for (u32 w = 0; w <= len && w < 10; w++)
                printf("      word %u : real 0x%08X   hle 0x%08X\n",
                       w, words[0][w], words[1][w]);
        ok &= same;
    }
    return ok;
}

int ac3_selftest_libgpu(const char *exe) {
    if (!exe || !*exe) exe = "game/ACE3.BIN";
    ac3_mem_init();
    if (!load_exe(exe)) return 1;
    MEM_W32(G_DRIVER_PTR, G_DRIVER_TABLE);
    MEM_W16(G_LIMIT_W, 1024);
    MEM_W16(G_LIMIT_H, 512);

    {
        extern void psyq_PutDispEnv(ctx_t *);
        ctx_t c;
        memset(&c, 0, sizeof c);
        c.r[29] = G_STACK;
        put_rect(G_ARG0, 0, 0, 512, 240);
        put_rect(G_ARG0 + 8, 0, 0, 0, 0);
        MEM_W32(G_ARG0 + 16, 0);
        c.r[4] = G_ARG0;
        psyq_PutDispEnv(&c);
    }

    u8 *after_real = (u8 *)malloc(AC3_RAM_SIZE);
    u8 *after_hle  = (u8 *)malloc(AC3_RAM_SIZE);
    u8 *baseline   = (u8 *)malloc(AC3_RAM_SIZE);
    if (!after_real || !after_hle || !baseline) return 1;

    printf("libgpu HLE equivalence -- real library code vs this port's handler\n\n");
    int ok = 1, ran = 0, unbound = 0;

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ac3_func_t hle = ac3_lookup(cases[i].addr);
        if (!hle) {
            printf("  %-14s not bound to a handler -- runs as recompiled MIPS\n",
                   cases[i].name);
            unbound++;
            continue;
        }

        ctx_t c;
        prepare(&c);
        memcpy(baseline, ac3_ram, AC3_RAM_SIZE);

        cases[i].setup(&c);
        if (!cases[i].hw) {
            ac3_interp_step_budget = 8000000u;
            ac3_interpret(&c, cases[i].addr);
            ac3_interp_step_budget = 0;
        }
        memcpy(after_real, ac3_ram, AC3_RAM_SIZE);
        u32 ret_real = c.r[2];

        memcpy(ac3_ram, baseline, AC3_RAM_SIZE);
        prepare(&c);
        cases[i].setup(&c);
        hle(&c);
        memcpy(after_hle, ac3_ram, AC3_RAM_SIZE);
        u32 ret_hle = c.r[2];

        ok &= compare(cases[i].name, after_real, after_hle, ret_real, ret_hle,
                      cases[i].ret, cases[i].hw,
                      cases[i].skip_lo, cases[i].skip_hi);
        ran++;
    }

    printf("\nDR_ENV packets -- the library's builder vs this port's\n\n");
    ok &= check_dr_env();

    printf("\n%d handlers compared", ran);
    if (unbound) printf(", %d unbound", unbound);
    printf("\n%s\n", ok ? "SELFTEST PASSED" : "SELFTEST FAILED");
    free(after_real); free(after_hle); free(baseline);
    return ok ? 0 : 1;
}
