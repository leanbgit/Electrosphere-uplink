/*
 * psyq_gpu.c -- the game's graphics library (libgpu): ordering tables and
 * drawing them, the drawing and display environments, and image transfers.
 * The packets themselves are decoded in gpu_packets.c; the pixels are drawn
 * by the software GPU (gpu_sw.c) and, in Enhanced mode, the HD renderer.
 */
#include "gpu_internal.h"
#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include "ac3_oracle.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

const gpu_backend_t *gpu = &gpu_backend_stats;

__attribute__((weak)) int ac3_render_quit_requested(void) { return 0; }
u16 *ac3_vram = NULL;

void gpu_select_backend(const gpu_backend_t *b) {
    if (gpu && gpu->shutdown) gpu->shutdown();
    gpu = b;
    if (gpu && gpu->init) gpu->init();
}

void ac3_vram_init(void) {
    if (!ac3_vram) ac3_vram = (u16 *)calloc(GPU_VRAM_W * GPU_VRAM_H, sizeof(u16));
}

static PSX_DRAWENV cur_draw;
static PSX_DISPENV cur_disp;
static u32 draw_count = 0;

void psyq_ResetGraph(ctx_t *ctx) {
    ac3_vram_init();
    u32 mode = A0 & 7u;
    if (mode == 0 || mode == 5) {
        gpu_sw_reset();
        gp0_in_polyline = 0;
        if (gpu->set_mask) gpu->set_mask(0, 0);
        if (gpu->set_draw_mode) gpu->set_draw_mode(0, 0, 0);
    }
    if (mode == 0 || mode == 3 || mode == 5) {
        memset(&cur_draw, 0xFF, sizeof cur_draw);
        memset(&cur_disp, 0xFF, sizeof cur_disp);
    }
    hle_trace("ResetGraph", "mode=%u", A0);
    RET(0);
}

void psyq_SetGraphDebug(ctx_t *ctx) { RET(0); }
void psyq_SetGraphQueue(ctx_t *ctx) { RET(0); }

void psyq_SetDispMask(ctx_t *ctx) {
    gpu_sw_display_enable(A0 != 0);
    RET(0);
}

void psyq_DrawSync(ctx_t *ctx) {
    RET(0);
}

void psyq_DrawSyncCallback(ctx_t *ctx) { RET(0); }
void psyq__GPU_ResetCallback(ctx_t *ctx) { RET(0); }

void psyq_GetODE(ctx_t *ctx) { RET(0); }

void psyq_GetTPage(ctx_t *ctx) {
    u32 tp = A0, abr = A1, x = A2, y = A3;
    u32 v = ((tp & 3u) << 7) | ((abr & 3u) << 5) |
            ((y & 0x100u) >> 4) | ((x & 0x3FFu) >> 6) |
            ((y & 0x200u) << 2);
    RET(v & 0xFFFFu);
}

void psyq_GetClut(ctx_t *ctx) {
    RET(((A1 << 6) | ((A0 >> 4) & 0x3Fu)) & 0xFFFFu);
}

#define LIBGPU_TAIL      0x800BBA24u
#define LIBGPU_TAIL_NEXT 0x800BBA10u

static void write_libgpu_tail(u32 end_slot) {
    MEM_W32(LIBGPU_TAIL, (4u << 24) | (LIBGPU_TAIL_NEXT & 0x00FFFFFFu));
    MEM_W32(LIBGPU_TAIL_NEXT, (4u << 24) | OT_TERM);
    MEM_W32(end_slot, LIBGPU_TAIL & 0x00FFFFFFu);
}

void psyq_ClearOTag(ctx_t *ctx) {
    u32 ot = A0;
    int n = (int)A1;
    for (int i = 0; i < n; i++) {
        u32 next = (i + 1 < n) ? ((ot + 4u * (i + 1)) & 0x00FFFFFFu) : OT_TERM;
        MEM_W32(ot + 4u * i, next);
    }
    if (n > 0) write_libgpu_tail(ot + 4u * (u32)(n - 1));
    RET(ot);
}

void psyq_ClearOTagR(ctx_t *ctx) {
    u32 ot = A0;
    int n = (int)A1;
    for (int i = 0; i < n; i++) {
        u32 next = (i > 0) ? ((ot + 4u * (i - 1)) & 0x00FFFFFFu) : OT_TERM;
        MEM_W32(ot + 4u * i, next);
    }
    if (n > 0) write_libgpu_tail(ot);
    RET(ot);
}

#include "coop.h"
/* Co-op, guest's computer: the frame is drawn twice. First the host's
 * picture into the game's VRAM (identical on both computers: the game reads
 * pixels back now and then), then the guest's picture into a display copy
 * with the same starting GPU state. The enhanced renderer's hooks only see
 * the guest's pass. See runtime/hle/coop.c. */
static void draw_ot(u32 head) {
    if (!ac3_coop_gpu_ready()) { walk_list(head); return; }   /* no guest picture this frame */
    gpu_sw_state_t before = gpu_sw;
#ifdef AC3_ENHANCED
    const gpu_sw_hooks_t *hooks = gpu_sw_hooks;
    gpu_sw_hooks = NULL;
#endif
    walk_list(head);
#ifdef AC3_ENHANCED
    gpu_sw_hooks = hooks;
#endif
    ac3_coop_gpu_draw(1);
    gpu_sw_state_t after = gpu_sw;
    u16 *game = ac3_vram, *disp = ac3_coop_display_pass(game);
    if (disp) {
        gpu_sw = before;
        ac3_vram = disp;
        walk_list(head);
        ac3_vram = game;
        gpu_sw = after;
    }
    ac3_coop_gpu_draw(0);
}

void psyq_DrawOTag(ctx_t *ctx) {
    draw_ot(A0);
    draw_count++;
    RET(0);
}

void psyq_DrawOTag2(ctx_t *ctx) {
    walk_list(A0);
    draw_count++;
    RET(0);
}

void psyq_DrawPrim(ctx_t *ctx) {
    u32 tag = MEM_R32(A0);
    int len = (int)OT_LEN(tag);
    if (len > 0) decode_packet(A0 + 4, len);
    RET(0);
}

static void draw_area_clamp(s32 *x, s32 *y) {
    if (*x < 0) *x = 0; else if (*x > GPU_VRAM_W - 1) *x = GPU_VRAM_W - 1;
    if (*y < 0) *y = 0; else if (*y > GPU_VRAM_H - 1) *y = GPU_VRAM_H - 1;
}

static u32 draw_area_word(u32 cmd, s32 x, s32 y) {
    draw_area_clamp(&x, &y);
    return (cmd << 24) | (((u32)y & 0x3FFu) << 10) | ((u32)x & 0x3FFu);
}

static u32 texture_window_word(u32 tw) {
    if (!tw) return 0;
    u32 xlo = MEM_R8U(tw + 0), ylo = MEM_R8U(tw + 2);
    s32 w = (s16)MEM_R16U(tw + 4), h = (s16)MEM_R16U(tw + 6);
    return (0xE2u << 24)
         | (((u32)ylo >> 3) << 15)
         | (((u32)xlo >> 3) << 10)
         | ((u32)(((-h) << 2)) & 0x3E0u)
         | (((u32)(-w) & 0xFFu) >> 3);
}

static u32 build_dr_env(u32 env_addr) {
    const PSX_DRAWENV *env = GUEST(env_addr, PSX_DRAWENV);
    u32 p = env_addr + (u32)offsetof(PSX_DRAWENV, dr_env);
    s16 cx = env->clip.x, cy = env->clip.y;
    s16 cw = env->clip.w, ch = env->clip.h;

    MEM_W32(p + 4,  draw_area_word(0xE3u, cx, cy));
    MEM_W32(p + 8,  draw_area_word(0xE4u, (s16)(cx + cw - 1), (s16)(cy + ch - 1)));
    MEM_W32(p + 12, (0xE5u << 24) | (((u32)env->ofs[1] & 0x7FFu) << 11) |
                    ((u32)env->ofs[0] & 0x7FFu));
    MEM_W32(p + 16, (0xE1u << 24) | (env->dtd ? 0x200u : 0u) |
                    (env->dfe ? 0x400u : 0u) | ((u32)env->tpage & 0x9FFu));
    MEM_W32(p + 20, texture_window_word(env_addr + 12u));
    MEM_W32(p + 24, 0xE6000000u);
    u32 len = 6;

    if (env->isbg) {
        s32 w = cw < 0 ? 0 : (cw > GPU_VRAM_W - 1 ? GPU_VRAM_W - 1 : cw);
        s32 h = ch < 0 ? 0 : (ch > GPU_VRAM_H - 1 ? GPU_VRAM_H - 1 : ch);
        u32 rgb = (u32)env->r0 | ((u32)env->g0 << 8) | ((u32)env->b0 << 16);
        u32 wh = ((u32)(u16)h << 16) | (u16)w;
        if ((cx & 0x3F) || (w & 0x3F)) {
            u16 tx = (u16)(cx - env->ofs[0]), ty = (u16)(cy - env->ofs[1]);
            MEM_W32(p + 28, 0x60000000u | rgb);
            MEM_W32(p + 32, ((u32)ty << 16) | tx);
        } else {
            MEM_W32(p + 28, 0x02000000u | rgb);
            MEM_W32(p + 32, ((u32)(u16)cy << 16) | (u16)cx);
        }
        MEM_W32(p + 36, wh);
        len = 9;
    }
    MEM_W8(p + 3, (u8)len);
    return p;
}

u32 ac3_gpu_build_dr_env(u32 env_addr) { return build_dr_env(env_addr); }

void psyq_DrawOTagEnv(ctx_t *ctx) {
    PSX_DRAWENV *env = GUEST(A1, PSX_DRAWENV);
    if (!env) { hle_warn("DrawOTagEnv", "bad pointer 0x%08X", A1); RET(0); return; }
    u32 p = build_dr_env(A1);
    MEM_W32(p, (MEM_R32(p) & 0xFF000000u) | (A0 & 0x00FFFFFFu));
    walk_list(p);
    draw_count++;
    cur_draw = *env;
    if (gpu->set_draw_env) gpu->set_draw_env(env);
    RET(0);
}

void psyq_PutDrawEnv(ctx_t *ctx) {
    PSX_DRAWENV *env = GUEST(A0, PSX_DRAWENV);
    if (!env) { hle_warn("PutDrawEnv", "bad pointer 0x%08X", A0); RET(A0); return; }
    u32 p = build_dr_env(A0);
    MEM_W32(p, MEM_R32(p) | 0x00FFFFFFu);
    walk_list(p);
    cur_draw = *env;
    if (gpu->set_draw_env) gpu->set_draw_env(env);
    RET(A0);
}

void psyq_GetDrawEnv(ctx_t *ctx) {
    PSX_DRAWENV *env = GUEST(A0, PSX_DRAWENV);
    if (env) *env = cur_draw;
    RET(A0);
}

void psyq_PutDispEnv(ctx_t *ctx) {
    PSX_DISPENV *env = GUEST(A0, PSX_DISPENV);
    if (env) {
        s32 w = env->disp.w, mw;
        if (w < 281)      mw = 256;
        else if (w < 353) mw = 320;
        else if (w < 401) mw = 368;
        else if (w < 561) mw = 512;
        else              mw = 640;
        gpu_sw_display_start(env->disp.x, env->disp.y);
        gpu_sw_display_mode(mw, env->disp.h >= 257 ? 480 : 240, env->isrgb24);
        cur_disp = *env;
        if (gpu->set_disp_env) gpu->set_disp_env(env);
    }
    RET(A0);
}

void psyq_GetDispEnv(ctx_t *ctx) {
    PSX_DISPENV *env = GUEST(A0, PSX_DISPENV);
    if (env) *env = cur_disp;
    RET(A0);
}

void psyq_SetDefDrawEnv(ctx_t *ctx) {
    PSX_DRAWENV *env = GUEST(A0, PSX_DRAWENV);
    if (env) {
        memset(env, 0, offsetof(PSX_DRAWENV, dr_env));
        env->clip.x = (s16)A1; env->clip.y = (s16)A2;
        env->clip.w = (s16)A3; env->clip.h = (s16)ASTACK(4);
        env->ofs[0] = (s16)A1; env->ofs[1] = (s16)A2;
        env->tw.x = env->tw.y = env->tw.w = env->tw.h = 0;
        env->dtd = 1;
        env->dfe = (env->clip.h < 257) ? 1 : 0;
        env->tpage = 10;
    }
    RET(A0);
}

void psyq_SetDefDispEnv(ctx_t *ctx) {
    PSX_DISPENV *env = GUEST(A0, PSX_DISPENV);
    if (env) {
        memset(env, 0, sizeof *env);
        env->disp.x = (s16)A1; env->disp.y = (s16)A2;
        env->disp.w = (s16)A3; env->disp.h = (s16)ASTACK(4);
        env->screen.x = env->screen.y = 0;
        env->screen.w = env->screen.h = 0;
        env->isinter = 0;
        env->isrgb24 = 0;
    }
    RET(A0);
}

static void set_tag_len(u32 pkt, u32 len) {
    u32 tag = MEM_R32(pkt);
    MEM_W32(pkt, (len << 24) | (tag & 0x00FFFFFFu));
}

void psyq_SetDrawArea(ctx_t *ctx) {
    PSX_RECT *r = GUEST(A1, PSX_RECT);
    if (r) {
        set_tag_len(A0, 2);
        MEM_W32(A0 + 4, draw_area_word(0xE3u, r->x, r->y));
        MEM_W32(A0 + 8, draw_area_word(0xE4u, r->x + r->w - 1,
                                              r->y + r->h - 1));
    }
    RET(A0);
}

void psyq_SetDrawOffset(ctx_t *ctx) {
    PSX_RECT *r = GUEST(A1, PSX_RECT);
    if (r) {
        set_tag_len(A0, 2);
        MEM_W32(A0 + 4, (0xE5u << 24) |
                        ((u32)(r->y & 0x7FF) << 11) | (u32)(r->x & 0x7FF));
        MEM_W32(A0 + 8, 0);
    }
    RET(A0);
}

void psyq_SetDrawMode(ctx_t *ctx) {
    u32 dfe = A1, dtd = A2, tpage = A3, tw = ASTACK(4);
    set_tag_len(A0, 2);
    MEM_W32(A0 + 4, (0xE1u << 24) | (tpage & 0x9FFu) |
                    ((dtd ? 1u : 0u) << 9) | ((dfe ? 1u : 0u) << 10));
    MEM_W32(A0 + 8, texture_window_word(tw));
    RET(A0);
}

void psyq_SetDrawTPage(ctx_t *ctx) {
    set_tag_len(A0, 1);
    MEM_W32(A0 + 4, (0xE1u << 24) | (A3 & 0x9FFu) |
                    ((A2 ? 1u : 0u) << 9) | ((A1 ? 1u : 0u) << 10));
    RET(A0);
}

void psyq_SetDrawLoad(ctx_t *ctx) {
    PSX_RECT *r = GUEST(A1, PSX_RECT);
    if (!r) { RET(A0); return; }
    s32 words = ((s32)r->w * (s32)r->h + 1) / 2;
    u32 len = (words < 13) ? (u32)(words + 4) : 0u;
    set_tag_len(A0, len);
    MEM_W32(A0 + 4, 0xA0000000u);
    MEM_W32(A0 + 8, MEM_R32(A1 + 0));
    MEM_W32(A0 + 12, MEM_R32(A1 + 4));
    MEM_W32(A0 + 4u * len, 0x01000000u);
    RET(A0);
}

void psyq_CatPrim(ctx_t *ctx) {
    u32 t0 = MEM_R32(A0);
    MEM_W32(A0, (t0 & 0xFF000000u) | (A1 & 0x00FFFFFFu));
    RET(A0);
}

static int clamp_transfer(PSX_RECT *r) {
    if (r->w < 0) r->w = 0; else if (r->w > GPU_VRAM_W) r->w = GPU_VRAM_W;
    if (r->h < 0) r->h = 0; else if (r->h > GPU_VRAM_H) r->h = GPU_VRAM_H;
    return ((s32)r->w * (s32)r->h + 1) / 2;
}

void psyq_LoadImage(ctx_t *ctx) {
    ac3_vram_init();
    PSX_RECT *r = GUEST(A0, PSX_RECT);
    if (!r) { hle_warn("LoadImage", "bad rect 0x%08X", A0); RET(0); return; }
    int words = clamp_transfer(r);
    if (words <= 0) { RET(0); return; }
    const u16 *src = (const u16 *)guest_ptr(A1, (u32)words * 4u);
    if (!src) { hle_warn("LoadImage", "bad source 0x%08X", A1); RET(0); return; }
    PSX_RECT d;
    gp0_load_begin(MEM_R32(A0), MEM_R32(A0 + 4), &d);
    for (int i = 0; i < words; i++) gpu_sw_load_word(MEM_R32(A1 + 4u * (u32)i));
    gpu_sw_load_cancel();
    ac3_capture_load_image(&d, A1);
    ac3_oracle_vram(AC3_OVRAM_UPLOAD, &d, A1, 0, 0);
    if (gpu->load_image) gpu->load_image(&d, src);
    RET(0);
}

void psyq_LoadImage2(ctx_t *ctx) { psyq_LoadImage(ctx); }

void psyq_StoreImage(ctx_t *ctx) {
    ac3_vram_init();
    PSX_RECT *r = GUEST(A0, PSX_RECT);
    if (!r) { RET(0); return; }
    int words = clamp_transfer(r);
    if (words <= 0) { RET(0); return; }
    u16 *dst = (u16 *)guest_ptr(A1, (u32)words * 4u);
    if (!dst) { hle_warn("StoreImage", "bad dest 0x%08X", A1); RET(0); return; }
    u32 xy = MEM_R32(A0), wh = MEM_R32(A0 + 4);
    PSX_RECT s = { GP0_POS_X(xy), GP0_POS_Y(xy >> 16),
                   GP0_COPY_W(wh), GP0_COPY_H(wh >> 16) };
    gpu_sw_store(&s, dst, (u32)words * 2u);
    if (gpu->store_image) gpu->store_image(&s, dst);
    ac3_oracle_vram(AC3_OVRAM_STORE, &s, A1, 0, 0);
    RET(0);
}

void psyq_StoreImage2(ctx_t *ctx) { psyq_StoreImage(ctx); }

void psyq_MoveImage(ctx_t *ctx) {
    ac3_vram_init();
    PSX_RECT *r = GUEST(A0, PSX_RECT);
    if (!r) { RET(0); return; }
    if (!r->w || !r->h) { RET((u32)-1); return; }
    gp0_copy(MEM_R32(A0), (A2 << 16) | (A1 & 0xFFFFu), MEM_R32(A0 + 4));
    RET(0);
}

void psyq_MoveImage2(ctx_t *ctx) { psyq_MoveImage(ctx); }

void psyq_ClearImage(ctx_t *ctx) {
    ac3_vram_init();
    PSX_RECT *r = GUEST(A0, PSX_RECT);
    if (!r) { RET(0); return; }
    if (r->w < 0) r->w = 0; else if (r->w > GPU_VRAM_W - 1) r->w = GPU_VRAM_W - 1;
    if (r->h < 0) r->h = 0; else if (r->h > GPU_VRAM_H - 1) r->h = GPU_VRAM_H - 1;
    u8 red = (u8)A1, green = (u8)A2, blue = (u8)A3;
    u32 rgb = (u32)red | ((u32)green << 8) | ((u32)blue << 16);
    u32 xy = MEM_R32(A0), wh = MEM_R32(A0 + 4);

    if (!(r->x & 0x3F) && !(r->w & 0x3F)) {
        gp0_env(0xE6000000u);
        gp0_env(0xE1000000u | (gpu_sw.texpage & 0x7FFu));
        gp0_fill(0x02000000u | rgb, xy, wh, "ClearImage");
    } else {
        u32 e3 = 0xE3000000u | ((u32)gpu_sw.area_y0 << 10) | (u32)gpu_sw.area_x0;
        u32 e4 = 0xE4000000u | ((u32)gpu_sw.area_y1 << 10) | (u32)gpu_sw.area_x1;
        u32 e5 = 0xE5000000u | (((u32)gpu_sw.ofs_y & 0x7FFu) << 11) |
                 ((u32)gpu_sw.ofs_x & 0x7FFu);
        gp0_env(0xE3000000u);
        gp0_env(0xE4FFFFFFu);
        gp0_env(0xE5000000u);
        gp0_env(0xE6000000u);
        gp0_env(0xE1000000u | (gpu_sw.texpage & 0x7FFu));
        gpu_rect_t t;
        memset(&t, 0, sizeof t);
        unpack_xy(xy, &t.x, &t.y);
        t.w = (u16)(wh & 0x3FFu);
        t.h = (u16)((wh >> 16) & 0x1FFu);
        t.r = red; t.g = green; t.b = blue;
        draw_rect(&t);
        gp0_env(e3);
        gp0_env(e4);
        gp0_env(e5);
        ac3_capture_fill_rect(r, red, green, blue, "ClearImage");
    }
    vram_mark_written(r, VRAM_W_FILL, red, green, blue);
    RET(0);
}

void psyq_ClearImage2(ctx_t *ctx) { psyq_ClearImage(ctx); }

u32 ac3_gpu_frame_count(void) { return draw_count; }

void ac3_gpu_env_snapshot(PSX_DISPENV *disp, PSX_DRAWENV *draw) {
    if (disp) *disp = cur_disp;
    if (draw) { *draw = cur_draw; draw->tpage = gpu_sw.texpage; }
}

void ac3_gpu_machine_save(FILE *f) {
    ac3_ms_tag(f, "GPU");
    ac3_ms_bytes(f, &cur_draw, (u32)sizeof cur_draw);
    ac3_ms_bytes(f, &cur_disp, (u32)sizeof cur_disp);
    ac3_ms_u32(f, draw_count);
}
