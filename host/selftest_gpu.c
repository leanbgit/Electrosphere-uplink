#include "gpu_backend.h"
#include <stdio.h>
#include <string.h>

extern void psyq_ClearOTagR(ctx_t *ctx);
extern void psyq_DrawOTag(ctx_t *ctx);
extern void psyq_PutDrawEnv(ctx_t *ctx);
extern void ac3_gpu_stats_dump(void);

#define OT_BASE   0x80100000u
#define PRIM_BASE 0x80110000u
#define OT_LEN_N  8

static struct {
    int polys, tris, quads, textured, gouraud, semitrans;
    int rects, lines;
    gpu_poly_t poly[8];
    gpu_rect_t rect[8];
} seen;

static void cap_poly(const gpu_poly_t *p) {
    if (seen.polys < 8) seen.poly[seen.polys] = *p;
    seen.polys++;
    if (p->nverts == 3) seen.tris++; else seen.quads++;
    if (p->textured) seen.textured++;
    if (p->gouraud) seen.gouraud++;
    if (p->semitrans) seen.semitrans++;
}
static void cap_rect(const gpu_rect_t *r) {
    if (seen.rects < 8) seen.rect[seen.rects] = *r;
    seen.rects++;
}
static void cap_line(const gpu_line_t *l) { (void)l; seen.lines++; }

static struct {
    int areas, offsets, masks, moves, fills;
    s16 area[4];
    s16 ofs[2];
    int mask_set, mask_check, mask_seen;
    s16 move_src[4], move_dst[2];
    s16 fill[4];
} state;

static void cap_area(s16 x0, s16 y0, s16 x1, s16 y1) {
    state.areas++;
    state.area[0] = x0; state.area[1] = y0;
    state.area[2] = x1; state.area[3] = y1;
}
static void cap_offset(s16 x, s16 y) {
    state.offsets++;
    state.ofs[0] = x; state.ofs[1] = y;
}
static void cap_mask(int set_bit, int check_bit) {
    state.masks++;
    state.mask_set = set_bit;
    state.mask_check = check_bit;
    state.mask_seen |= 1 << ((set_bit ? 1 : 0) | (check_bit ? 2 : 0));
}
static void cap_move(const PSX_RECT *r, s16 dx, s16 dy) {
    state.moves++;
    state.move_src[0] = r->x; state.move_src[1] = r->y;
    state.move_src[2] = r->w; state.move_src[3] = r->h;
    state.move_dst[0] = dx;   state.move_dst[1] = dy;
}
static void cap_fill(const PSX_RECT *r, u8 red, u8 green, u8 blue) {
    (void)red; (void)green; (void)blue;
    state.fills++;
    state.fill[0] = r->x; state.fill[1] = r->y;
    state.fill[2] = r->w; state.fill[3] = r->h;
}

static const gpu_backend_t capture_backend = {
    "capture", NULL, NULL, NULL, NULL,
    NULL, NULL, cap_area, cap_offset, NULL, cap_mask,
    cap_poly, cap_rect, cap_line,
    NULL, NULL, cap_move, cap_fill,
};

static u32 prim_cursor;

static u32 emit(const u32 *words, int n, u8 code, int len) {
    u32 at = prim_cursor;
    MEM_W32(at, ((u32)len << 24) | OT_TERM);
    for (int i = 0; i < n; i++) MEM_W32(at + 4u + 4u * i, words[i]);
    u32 w0 = MEM_R32(at + 4u);
    MEM_W32(at + 4u, (w0 & 0x00FFFFFFu) | ((u32)code << 24));
    prim_cursor += 4u + 4u * (u32)len;
    return at;
}

static u32 xy(int x, int y) { return ((u32)(y & 0xFFFF) << 16) | (u32)(x & 0xFFFF); }
static u32 rgb(int r, int g, int b) {
    return ((u32)(b & 0xFF) << 16) | ((u32)(g & 0xFF) << 8) | (u32)(r & 0xFF);
}

static void add_prim(u32 ot, int slot, u32 prim) {
    u32 slot_addr = ot + 4u * (u32)slot;
    u32 slot_tag = MEM_R32(slot_addr);
    u32 prim_tag = MEM_R32(prim);
    MEM_W32(prim, (prim_tag & 0xFF000000u) | (slot_tag & 0x00FFFFFFu));
    MEM_W32(slot_addr, (slot_tag & 0xFF000000u) | (prim & 0x00FFFFFFu));
}

static int check(const char *what, long got, long want) {
    int ok = (got == want);
    printf("  %-34s %-6ld %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) printf("      expected %ld\n", want);
    return ok;
}

static int selftest_multi_command(void) {
#define OT2_BASE  0x80120000u
#define OT2_LEN_N 8
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x80130000u;

    ctx.r[4] = OT2_BASE;
    ctx.r[5] = OT2_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 area[] = {
        0xE3000000u | ((u32)(20 & 0x1FF) << 10) | (12u & 0x3FF),
        0xE4000000u | ((u32)(77 & 0x1FF) << 10) | (75u & 0x3FF),
    };
    u32 p_area = emit(area, 2, 0xE3, 2);

    u32 env[] = {
        0xE3000000u,
        0xE4000000u | ((u32)(239 & 0x1FF) << 10) | (319u & 0x3FF),
        0xE5000000u | ((u32)(120 & 0x7FF) << 11) | (160u & 0x7FF),
        0x00000000u,
    };
    u32 p_env = emit(env, 4, 0xE3, 4);

    u32 stream[] = {
        0xE6000001u,
        0x01000000u,
        0x80000000u,
        ((u32)240 << 16) | 32u,
        ((u32)8 << 16) | 800u,
        ((u32)4 << 16) | 4u,
        0xE6000000u,
    };
    u32 p_stream = emit(stream, 7, 0xE6, 7);

    u32 masked[] = {
        0xE6000002u,
        0x60000000u | rgb(0x40, 0x50, 0x60),
        xy(3, 4), xy(16, 16),
    };
    u32 p_masked = emit(masked, 4, 0xE6, 4);

    ac3_vram_init();
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            ac3_vram[(240 + y) * GPU_VRAM_W + (32 + x)] = (u16)(0x1000 + y * 4 + x);
            ac3_vram[(8 + y) * GPU_VRAM_W + (800 + x)] = 0xFFFF;
        }

    add_prim(OT2_BASE, 0, p_area);
    add_prim(OT2_BASE, 1, p_env);
    add_prim(OT2_BASE, 2, p_stream);
    add_prim(OT2_BASE, 3, p_masked);

    ctx.r[4] = OT2_BASE + 4u * (OT2_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- multi-command packets ---\n");
    int ok = 1;
    ok &= check("set_draw_area calls", state.areas, 2);
    ok &= check("  last area x0", state.area[0], 12);
    ok &= check("  last area y0", state.area[1], 20);
    ok &= check("  last area x1", state.area[2], 76);
    ok &= check("  last area y1", state.area[3], 78);
    ok &= check("set_draw_offset calls", state.offsets, 1);
    ok &= check("  offset x", state.ofs[0], 160);
    ok &= check("  offset y", state.ofs[1], 120);
    ok &= check("set_mask calls", state.masks, 3);
    ok &= check("  modes seen", state.mask_seen, 0x7);
    ok &= check("  mask after last E6", state.mask_set, 0);
    ok &= check("MoveImage packets", state.moves, 1);
    ok &= check("  source x", state.move_src[0], 32);
    ok &= check("  source y", state.move_src[1], 240);
    ok &= check("  width", state.move_src[2], 4);
    ok &= check("  dest x", state.move_dst[0], 800);
    ok &= check("  dest y", state.move_dst[1], 8);
    ok &= check("  VRAM moved, mask bit set", ac3_vram[8 * GPU_VRAM_W + 800], 0x9000);
    ok &= check("  VRAM moved (last)", ac3_vram[11 * GPU_VRAM_W + 803], 0x900F);
    ok &= check("primitive behind a state word", seen.rects, 1);
    ok &= check("  its x", seen.rect[0].x, 3);
    ok &= check("  its w", seen.rect[0].w, 16);
    return ok;
}

static void put_frame_env(ctx_t *ctx, u32 env) {
    for (int i = 0; i < 23; i++) MEM_W32(env + 4u * (u32)i, 0);
    MEM_W16(env + 0, 0);   MEM_W16(env + 2, 0);
    MEM_W16(env + 4, 320); MEM_W16(env + 6, 240);
    ctx->r[4] = env;
    psyq_PutDrawEnv(ctx);
}

static int selftest_heat_haze(void) {
#define OT3_BASE  0x80140000u
#define OT3_LEN_N 4
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x80150000u;
    ac3_vram_init();
    put_frame_env(&ctx, 0x80160000u);

    ctx.r[4] = OT3_BASE;
    ctx.r[5] = OT3_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 ramp[] = { rgb(0, 0, 0), xy(40, 100), rgb(255, 0, 0), xy(56, 100),
                   rgb(0, 0, 0), xy(40, 101), rgb(255, 0, 0), xy(56, 101) };
    u32 p_ramp = emit(ramp, 8, 0x38, 8);
    u32 bg[] = { rgb(0, 0, 0xF8), xy(200, 50), xy(16, 1) };
    u32 p_bg = emit(bg, 3, 0x60, 3);

    static const u8 idx[16] = { 3, 1, 4, 15, 9, 2, 6, 5, 0, 7, 11, 8, 12, 10, 13, 14 };
    for (int k = 0; k < 4; k++)
        ac3_vram[256 * GPU_VRAM_W + 448 + k] =
            (u16)(idx[4 * k] | (idx[4 * k + 1] << 4) |
                  (idx[4 * k + 2] << 8) | (idx[4 * k + 3] << 12));
    for (int i = 0; i < 16; i++) ac3_vram[299 * GPU_VRAM_W + 352 + i] = 0x1234;

    const u32 clut = (299u << 6) | (352u >> 4);
    u32 haze[] = {
        0xE6000001u,
        0xE1000017u,
        0x01000000u,
        0x80000000u, xy(40, 100), xy(352, 299), xy(16, 1),
        0x67000000u, xy(200, 50), clut << 16, xy(16, 1),
        0xE6000000u,
    };
    u32 p_haze = emit(haze, 12, 0xE6, 12);

    add_prim(OT3_BASE, 2, p_ramp);
    add_prim(OT3_BASE, 1, p_bg);
    add_prim(OT3_BASE, 0, p_haze);
    ctx.r[4] = OT3_BASE + 4u * (OT3_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- the heat haze: copying what was drawn ---\n");
    int ok = 1;
    const u16 *frame = &ac3_vram[100 * GPU_VRAM_W + 40];
    const u16 *pal = &ac3_vram[299 * GPU_VRAM_W + 352];
    ok &= check("the ramp was drawn into VRAM", frame[15] != frame[0], 1);
    int copied = 1;
    for (int i = 0; i < 16; i++) copied &= pal[i] == (u16)(frame[i] | 0x8000u);
    ok &= check("palette = drawn strip | mask bit", copied, 1);
    ok &= check("  black pixel kept visible", pal[0], 0x8000);

    int drawn = 1;
    for (int u = 0; u < 16; u++) {
        u16 t = pal[idx[u]], b = 0x7C00;
        u16 want = (u16)(0x8000u
                 | ((((t & 31) + (b & 31)) >> 1))
                 | (((((t >> 5) & 31) + ((b >> 5) & 31)) >> 1) << 5)
                 | (((((t >> 10) & 31) + ((b >> 10) & 31)) >> 1) << 10));
        if (ac3_vram[50 * GPU_VRAM_W + 200 + u] != want) {
            printf("      sprite x=%d: got 0x%04X want 0x%04X\n", u,
                   ac3_vram[50 * GPU_VRAM_W + 200 + u], want);
            drawn = 0;
        }
    }
    ok &= check("sprite = shuffled frame, averaged", drawn, 1);
    ok &= check("copies reaching the backend", state.moves, 1);
    return ok;
}

static int selftest_transfer_masks(void) {
#define OT4_BASE  0x80170000u
#define OT4_LEN_N 4
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x80180000u;
    ac3_vram_init();

    for (int i = 0; i < 16; i++)
        ac3_vram[100 * GPU_VRAM_W + 600 + i] = (u16)(0x4000 + i);
    for (int i = 0; i < 16; i++)
        ac3_vram[2 * GPU_VRAM_W + 600 + i] = 0xFFFF;

    u32 env = 0x80190000u;
    for (int i = 0; i < 20; i++) MEM_W32(env + 4u * (u32)i, 0);
    MEM_W16(env + 0, 0);   MEM_W16(env + 2, 0);
    MEM_W16(env + 4, 320); MEM_W16(env + 6, 240);
    ctx.r[4] = env;
    psyq_PutDrawEnv(&ctx);

    ctx.r[4] = OT4_BASE;
    ctx.r[5] = OT4_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 runaway[] = { 0x02020202u, 0x02020202u, 0x02020202u };
    u32 p_runaway = emit(runaway, 3, 0x02, 3);

    u32 clear[] = { 0x02000000u | rgb(8, 16, 24), xy(16, 32), xy(100, 50) };
    u32 p_clear = emit(clear, 3, 0x02, 3);

    u32 load[] = {
        0xA0000000u, 0xF1F4F250u, 0xFC02FC04u,
        0xBBBBAAAAu, 0xDDDDCCCCu, 0x22221111u, 0x44443333u,
        0x60000000u | rgb(1, 2, 3), xy(9, 11), xy(20, 21),
    };
    u32 p_load = emit(load, 10, 0xA0, 10);

    add_prim(OT4_BASE, 0, p_runaway);
    add_prim(OT4_BASE, 1, p_clear);
    add_prim(OT4_BASE, 2, p_load);

    ctx.r[4] = OT4_BASE + 4u * (OT4_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- VRAM transfer field widths ---\n");
    int ok = 1;
    ok &= check("fills reaching the backend", state.fills, 2);
    ok &= check("  runaway fill x", state.fill[0], 512);
    ok &= check("  runaway fill y", state.fill[1], 2);
    ok &= check("  runaway fill w", state.fill[2], 528);
    ok &= check("  runaway fill h", state.fill[3], 2);
    ok &= check("texture page survived", ac3_vram[100 * GPU_VRAM_W + 600], 0x4000);
    ok &= check("  ...to its last texel", ac3_vram[100 * GPU_VRAM_W + 615], 0x400F);
    ok &= check("the two rows it does cover", ac3_vram[2 * GPU_VRAM_W + 600], 0);

    ok &= check("load landed at x", ac3_vram[500 * GPU_VRAM_W + 592], 0xAAAA);
    ok &= check("  ...last pixel of row 0", ac3_vram[500 * GPU_VRAM_W + 595], 0xDDDD);
    ok &= check("  ...first pixel of row 1", ac3_vram[501 * GPU_VRAM_W + 592], 0x1111);
    ok &= check("command behind the load ran", seen.rects, 1);
    ok &= check("  its x", seen.rect[0].x, 9);
    ok &= check("  its w", seen.rect[0].w, 20);
    return ok;
}

static int selftest_vram_wrap(void) {
#define OT5_BASE  0x801A0000u
#define OT5_LEN_N 4
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x801B0000u;
    ac3_vram_init();
    put_frame_env(&ctx, 0x801C0000u);

    for (int i = 0; i < 24; i++) {
        ac3_vram[400 * GPU_VRAM_W + 1000 + i] = (u16)(0x6000 + i);
        ac3_vram[400 * GPU_VRAM_W + i]        = (u16)(0x7000 + i);
    }
    ac3_vram[510 * GPU_VRAM_W + 100] = 0x0510;
    ac3_vram[511 * GPU_VRAM_W + 100] = 0x0511;
    ac3_vram[  0 * GPU_VRAM_W + 100] = 0x0000 + 0x0A00;
    ac3_vram[  1 * GPU_VRAM_W + 100] = 0x0001 + 0x0A00;

    ctx.r[4] = OT5_BASE;
    ctx.r[5] = OT5_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 wrapped[] = { 0x80000000u, ((u32)400 << 16) | 1000u,
                      ((u32)450 << 16) |  600u, ((u32)2 << 16) | 48u };
    u32 p_wrapped = emit(wrapped, 4, 0x80, 4);
    u32 down[] = { 0x80000000u, ((u32)510 << 16) | 100u,
                   ((u32)200 << 16) | 700u, ((u32)4 << 16) | 1u };
    u32 p_down = emit(down, 4, 0x80, 4);

    add_prim(OT5_BASE, 0, p_wrapped);
    add_prim(OT5_BASE, 1, p_down);
    ctx.r[4] = OT5_BASE + 4u * (OT5_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- VRAM wraps, it does not end ---\n");
    int ok = 1;
    ok &= check("copies reaching the backend", state.moves, 2);
    ok &= check("wrapped copy, before the edge", ac3_vram[450 * GPU_VRAM_W + 600],
                0x6000);
    ok &= check("  ...last column before it", ac3_vram[450 * GPU_VRAM_W + 623],
                0x6017);
    ok &= check("  ...first column after it", ac3_vram[450 * GPU_VRAM_W + 624],
                0x7000);
    ok &= check("  ...and on to the end", ac3_vram[450 * GPU_VRAM_W + 647], 0x7017);
    ok &= check("down the bottom edge, row 510", ac3_vram[200 * GPU_VRAM_W + 700],
                0x0510);
    ok &= check("  ...row 0 after it", ac3_vram[202 * GPU_VRAM_W + 700], 0x0A00);
    ok &= check("  ...row 1", ac3_vram[203 * GPU_VRAM_W + 700], 0x0A01);
    return ok;
}

static int selftest_fill(void) {
#define OT6_BASE  0x801A0000u
#define OT6_LEN_N 4
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x801B0000u;
    ac3_vram_init();
    put_frame_env(&ctx, 0x801C0000u);

    for (int i = 0; i < 64; i++) ac3_vram[12 * GPU_VRAM_W + 720 + i] = 0x1111;
    ac3_vram[13 * GPU_VRAM_W + 740] = 0x8123;
    for (int i = 0; i < 8; i++) {
        ac3_vram[20 * GPU_VRAM_W + i] = 0x2222;
        ac3_vram[20 * GPU_VRAM_W + 1016 + i] = 0x2222;
    }

    ctx.r[4] = OT6_BASE;
    ctx.r[5] = OT6_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 away[] = { 0xE6000003u,
                   0x02000000u | rgb(8, 16, 24), xy(741, 12), xy(20, 2),
                   0xE6000000u };
    u32 p_away = emit(away, 5, 0xE6, 5);
    u32 edge[] = { rgb(255, 255, 255), xy(1008, 20), xy(32, 1) };
    u32 p_edge = emit(edge, 3, 0x02, 3);

    add_prim(OT6_BASE, 0, p_away);
    add_prim(OT6_BASE, 1, p_edge);
    ctx.r[4] = OT6_BASE + 4u * (OT6_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- fills write VRAM, not the frame ---\n");
    int ok = 1;
    ok &= check("fills reaching the backend", state.fills, 2);
    ok &= check("x rounded down to 736", ac3_vram[12 * GPU_VRAM_W + 736], 0x0C41);
    ok &= check("  ...not before it", ac3_vram[12 * GPU_VRAM_W + 735], 0x1111);
    ok &= check("width rounded up to 32", ac3_vram[12 * GPU_VRAM_W + 767], 0x0C41);
    ok &= check("  ...not past it", ac3_vram[12 * GPU_VRAM_W + 768], 0x1111);
    ok &= check("masked pixel overwritten, bit clear",
                ac3_vram[13 * GPU_VRAM_W + 740], 0x0C41);
    ok &= check("wrapped: last column", ac3_vram[20 * GPU_VRAM_W + 1023], 0x7FFF);
    ok &= check("  ...first column after", ac3_vram[20 * GPU_VRAM_W + 0], 0x7FFF);
    ok &= check("  ...and on to x=15", ac3_vram[20 * GPU_VRAM_W + 15], 0x7FFF);
    return ok;
}

static int selftest_gp0_stream(void) {
#define OT7_BASE  0x801A8000u
#define OT7_LEN_N 8
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    memset(&state, 0, sizeof state);
    prim_cursor = 0x801B8000u;
    ac3_vram_init();

    for (int i = 0; i < 16; i++)
        ac3_vram[12 * GPU_VRAM_W + 736 + i] = (u16)(0x3000 + i);

    ctx.r[4] = OT7_BASE;
    ctx.r[5] = OT7_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 load[] = { 0xA0000000u, ((u32)300 << 16) | 900u, ((u32)4 << 16) | 4u,
                   0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u };
    u32 p_load = emit(load, 7, 0xA0, 7);
    u32 tail[] = { rgb(86, 0, 0), xy(736, 12), xy(672, 148), 0xDEADBEEFu };
    u32 p_tail = emit(tail, 4, 0x02, 4);
    u32 tile1[] = { rgb(1, 2, 3), xy(5, 6), xy(20, 21) };
    u32 p_tile1 = emit(tile1, 3, 0x60, 3);

    u32 poly[] = { rgb(9, 9, 9), xy(10, 10), xy(20, 20) };
    u32 p_poly = emit(poly, 3, 0x48, 3);
    u32 rest[] = { rgb(86, 0, 0), xy(736, 12), xy(672, 148), 0x55555555u };
    u32 p_rest = emit(rest, 4, 0x02, 4);
    u32 tile2[] = { rgb(4, 5, 6), xy(7, 8), xy(30, 31) };
    u32 p_tile2 = emit(tile2, 3, 0x60, 3);

    add_prim(OT7_BASE, 5, p_load);
    add_prim(OT7_BASE, 4, p_tail);
    add_prim(OT7_BASE, 3, p_tile1);
    add_prim(OT7_BASE, 2, p_poly);
    add_prim(OT7_BASE, 1, p_rest);
    add_prim(OT7_BASE, 0, p_tile2);

    ctx.r[4] = OT7_BASE + 4u * (OT7_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("  --- GP0 states that outlive a node ---\n");
    int ok = 1;
    ok &= check("fills reaching the backend", state.fills, 0);
    ok &= check("the fill's target survived", ac3_vram[12 * GPU_VRAM_W + 736],
                0x3000);
    ok &= check("  ...to its last texel", ac3_vram[12 * GPU_VRAM_W + 751],
                0x300F);
    ok &= check("image data still uploaded", ac3_vram[300 * GPU_VRAM_W + 900],
                0x2222);
    ok &= check("rectangles decoded after both", seen.rects, 2);
    ok &= check("  the one behind the transfer", seen.rect[0].x, 5);
    ok &= check("  the one behind the polyline", seen.rect[1].x, 7);
    return ok;
}

int ac3_selftest_gpu(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&seen, 0, sizeof seen);
    ac3_vram_init();
    gpu_select_backend(&capture_backend);
    prim_cursor = PRIM_BASE;

    ctx.r[4] = OT_BASE;
    ctx.r[5] = OT_LEN_N;
    psyq_ClearOTagR(&ctx);

    u32 f3[] = { rgb(0x11, 0x22, 0x33), xy(10, 20), xy(30, 40), xy(50, 60) };
    u32 p_f3 = emit(f3, 4, 0x20, 4);

    u32 gt4[] = {
        rgb(1, 2, 3),      xy(100, 100), (0x1234u << 16) | (8u << 8) | 4u,
        rgb(4, 5, 6),      xy(200, 100), (0x5678u << 16) | (9u << 8) | 5u,
        rgb(7, 8, 9),      xy(100, 200), (6u << 8) | 10u,
        rgb(10, 11, 12),   xy(200, 200), (7u << 8) | 11u,
    };
    u32 p_gt4 = emit(gt4, 12, 0x3C, 12);

    u32 sprt[] = { rgb(0x80, 0x80, 0x80), xy(5, 6),
                   (0x4321u << 16) | (32u << 8) | 16u, xy(64, 48) };
    u32 p_sprt = emit(sprt, 4, 0x64, 4);

    u32 tile[] = { rgb(0xFF, 0x00, 0x00), xy(7, 8), xy(24, 12) };
    u32 p_tile = emit(tile, 3, 0x60, 3);

    u32 f4[] = { rgb(9, 9, 9), xy(1, 1), xy(2, 2), xy(3, 3), xy(4, 4) };
    u32 p_f4 = emit(f4, 5, 0x2A, 5);

    add_prim(OT_BASE, 0, p_f3);
    add_prim(OT_BASE, 1, p_gt4);
    add_prim(OT_BASE, 2, p_sprt);
    add_prim(OT_BASE, 3, p_tile);
    add_prim(OT_BASE, 4, p_f4);

    ctx.r[4] = OT_BASE + 4u * (OT_LEN_N - 1);
    psyq_DrawOTag(&ctx);

    printf("OT walker self-test\n");
    int ok = 1;
    ok &= check("primitives decoded", seen.polys + seen.rects, 5);
    ok &= check("polygons", seen.polys, 3);
    ok &= check("  triangles", seen.tris, 1);
    ok &= check("  quads", seen.quads, 2);
    ok &= check("  textured", seen.textured, 1);
    ok &= check("  gouraud", seen.gouraud, 1);
    ok &= check("  semi-transparent", seen.semitrans, 1);
    ok &= check("rectangles (SPRT+TILE)", seen.rects, 2);

    printf("  --- draw order (reverse of insertion) ---\n");
    ok &= check("poly[0] nverts (F4)", seen.poly[0].nverts, 4);
    ok &= check("poly[0] semitrans", seen.poly[0].semitrans, 1);
    ok &= check("poly[2] nverts (F3)", seen.poly[2].nverts, 3);

    const gpu_poly_t *q = &seen.poly[1];
    printf("  --- POLY_GT4 field decode ---\n");
    ok &= check("gouraud quad nverts", q->nverts, 4);
    ok &= check("  textured", q->textured, 1);
    ok &= check("  gouraud", q->gouraud, 1);
    ok &= check("  vertex0 x", q->x[0], 100);
    ok &= check("  vertex3 y", q->y[3], 200);
    ok &= check("  vertex0 rgb.r", q->r[0], 1);
    ok &= check("  vertex3 rgb.b", q->b[3], 12);
    ok &= check("  u0/v0", (q->u[0] << 8) | q->v[0], (4 << 8) | 8);
    ok &= check("  clut (from vertex0)", q->clut, 0x1234);
    ok &= check("  tpage (from vertex1)", q->tpage, 0x5678);

    printf("  --- rectangle decode ---\n");
    ok &= check("rect[0] is TILE w", seen.rect[0].w, 24);
    ok &= check("rect[0] is TILE h", seen.rect[0].h, 12);
    ok &= check("rect[0] textured", seen.rect[0].textured, 0);
    ok &= check("rect[1] is SPRT w", seen.rect[1].w, 64);
    ok &= check("rect[1] is SPRT h", seen.rect[1].h, 48);
    ok &= check("rect[1] textured", seen.rect[1].textured, 1);
    ok &= check("rect[1] clut", seen.rect[1].clut, 0x4321);

    ok &= selftest_multi_command();
    ok &= selftest_heat_haze();
    ok &= selftest_transfer_masks();
    ok &= selftest_vram_wrap();
    ok &= selftest_fill();
    ok &= selftest_gp0_stream();

    printf("%s\n", ok ? "SELFTEST PASSED" : "SELFTEST FAILED");
    return ok ? 0 : 1;
}
