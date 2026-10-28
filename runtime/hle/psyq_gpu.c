#include "gpu_backend.h"
#include "gpu_sw.h"
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

static inline void unpack_xy(u32 w, s16 *x, s16 *y) {
    *x = (s16)(w & 0xFFFF);
    *y = (s16)(w >> 16);
}

static inline void unpack_rgb(u32 w, u8 *r, u8 *g, u8 *b) {
    *r = (u8)(w & 0xFF);
    *g = (u8)((w >> 8) & 0xFF);
    *b = (u8)((w >> 16) & 0xFF);
}

#define VRAM_BLK      16
#define VRAM_BLK_W    (GPU_VRAM_W / VRAM_BLK)
#define VRAM_BLK_H    (GPU_VRAM_H / VRAM_BLK)

extern u32 ac3_vblank_count(void);

enum { VRAM_W_NONE = 0, VRAM_W_UPLOAD, VRAM_W_FILL, VRAM_W_COPY };
typedef struct {
    u32 frame;
    s16 x, y, w, h;
    u8  kind;
    u8  r, g, b;
} vram_write_t;
static vram_write_t vram_written[VRAM_BLK_W * VRAM_BLK_H];

u32 ac3_vram_last_write(int x, int y) {
    if (x < 0 || x >= GPU_VRAM_W || y < 0 || y >= GPU_VRAM_H) return 0;
    return vram_written[(y / VRAM_BLK) * VRAM_BLK_W + (x / VRAM_BLK)].frame;
}

int ac3_vram_last_write_info(int x, int y, u32 *frame, const char **what,
                             int *rx, int *ry, int *rw, int *rh,
                             u8 *rr, u8 *gg, u8 *bb) {
    static const char *names[] = { "nothing", "an upload", "a fill", "a copy" };
    if (x < 0 || x >= GPU_VRAM_W || y < 0 || y >= GPU_VRAM_H) return 0;
    const vram_write_t *w =
        &vram_written[(y / VRAM_BLK) * VRAM_BLK_W + (x / VRAM_BLK)];
    if (!w->kind) return 0;
    if (frame) *frame = w->frame;
    if (what)  *what  = names[w->kind < 4 ? w->kind : 0];
    if (rx) *rx = w->x;
    if (ry) *ry = w->y;
    if (rw) *rw = w->w;
    if (rh) *rh = w->h;
    if (rr) *rr = w->r;
    if (gg) *gg = w->g;
    if (bb) *bb = w->b;
    return 1;
}

static void vram_mark_written(const PSX_RECT *r, u8 kind,
                              u8 red, u8 green, u8 blue) {
    if (!r || r->w <= 0 || r->h <= 0) return;
    vram_write_t n;
    n.frame = ac3_vblank_count();
    n.x = r->x; n.y = r->y; n.w = r->w; n.h = r->h;
    n.kind = kind; n.r = red; n.g = green; n.b = blue;
    int bx0 = (r->x >= 0 ? r->x : r->x - (VRAM_BLK - 1)) / VRAM_BLK;
    int by0 = (r->y >= 0 ? r->y : r->y - (VRAM_BLK - 1)) / VRAM_BLK;
    int nbx = (r->x + r->w - 1 - bx0 * VRAM_BLK) / VRAM_BLK + 1;
    int nby = (r->y + r->h - 1 - by0 * VRAM_BLK) / VRAM_BLK + 1;
    if (nbx > VRAM_BLK_W) nbx = VRAM_BLK_W;
    if (nby > VRAM_BLK_H) nby = VRAM_BLK_H;
    for (int y = 0; y < nby; y++) {
        int by = (int)((unsigned)(by0 + y) & (VRAM_BLK_H - 1));
        for (int x = 0; x < nbx; x++) {
            int bx = (int)((unsigned)(bx0 + x) & (VRAM_BLK_W - 1));
            vram_written[by * VRAM_BLK_W + bx] = n;
        }
    }
}

static void check_clut(u16 clut) {
    static u16 last = 0xFFFF;
    if (clut == last) return;
    last = clut;
    if (!ac3_vram) return;
    int cx = (clut & 0x3F) * 16, cy = (clut >> 6) & 0x1FF;
    if (cx + 16 > GPU_VRAM_W || cy >= GPU_VRAM_H) return;
    for (int i = 0; i < 16; i++)
        if (*vram_at(cx + i, cy)) return;
    hle_warn("check_clut", "primitive samples the palette at (%d,%d), which is "
             "entirely zero -- it will draw nothing. VRAM there was last "
             "written on frame %u", cx, cy, ac3_vram_last_write(cx, cy));
}

static void decode_poly(u32 addr, u8 code) {
    gpu_poly_t p;
    memset(&p, 0, sizeof p);
    p.nverts = (code & 0x08) ? 4 : 3;
    p.textured = (code & 0x04) != 0;
    p.gouraud = (code & 0x10) != 0;
    p.semitrans = (code & 0x02) != 0;
    p.raw = (code & 0x01) != 0;

    u32 a = addr;
    u32 first = MEM_R32(a); a += 4;
    unpack_rgb(first, &p.r[0], &p.g[0], &p.b[0]);

    for (int i = 0; i < p.nverts; i++) {
        if (p.gouraud && i > 0) {
            u32 c = MEM_R32(a); a += 4;
            unpack_rgb(c, &p.r[i], &p.g[i], &p.b[i]);
        } else if (i > 0) {
            p.r[i] = p.r[0]; p.g[i] = p.g[0]; p.b[i] = p.b[0];
        }
        u32 xy = MEM_R32(a); a += 4;
        unpack_xy(xy, &p.x[i], &p.y[i]);
        if (p.textured) {
            u32 uv = MEM_R32(a); a += 4;
            p.u[i] = (u8)(uv & 0xFF);
            p.v[i] = (u8)((uv >> 8) & 0xFF);
            if (i == 0) p.clut = (u16)(uv >> 16);
            if (i == 1) p.tpage = (u16)(uv >> 16);
        }
    }
    if (p.textured) gpu_sw_texpage_attr(p.tpage);
    else            p.tpage = gpu_sw.texpage;
    gpu_sw_texwin(&p.twin_mask, &p.twin_off);
    if (p.textured && ((p.tpage >> 7) & 3) < 2) check_clut(p.clut);
    gpu_sw_poly(&p);
    ac3_capture_poly(&p);
    if (gpu->poly) gpu->poly(&p);
}

static void draw_rect(gpu_rect_t *r) {
    r->tpage = gpu_sw.texpage;
    gpu_sw_texwin(&r->twin_mask, &r->twin_off);
    if (r->textured && ((r->tpage >> 7) & 3) < 2) check_clut(r->clut);
    gpu_sw_rect(r);
    ac3_capture_rect(r);
    if (gpu->rect) gpu->rect(r);
}

static void decode_rect(u32 addr, u8 code) {
    gpu_rect_t r;
    memset(&r, 0, sizeof r);
    r.textured = (code & 0x04) != 0;
    r.semitrans = (code & 0x02) != 0;
    r.raw = (code & 0x01) != 0;

    u32 a = addr;
    u32 first = MEM_R32(a); a += 4;
    unpack_rgb(first, &r.r, &r.g, &r.b);
    u32 xy = MEM_R32(a); a += 4;
    unpack_xy(xy, &r.x, &r.y);
    if (r.textured) {
        u32 uv = MEM_R32(a); a += 4;
        r.u = (u8)(uv & 0xFF);
        r.v = (u8)((uv >> 8) & 0xFF);
        r.clut = (u16)(uv >> 16);
    }
    switch ((code >> 3) & 3) {
    case 0: {
        u32 wh = MEM_R32(a);
        r.w = (u16)(wh & 0x3FFu);
        r.h = (u16)((wh >> 16) & 0x1FFu);
        break;
    }
    case 1: r.w = r.h = 1; break;
    case 2: r.w = r.h = 8; break;
    case 3: r.w = r.h = 16; break;
    }
    draw_rect(&r);
}

static inline int gp0_polyline_end(u32 w) {
    return (w & 0xF000F000u) == 0x50005000u;
}

static void decode_line(u32 addr, u8 code, int len) {
    gpu_line_t l;
    memset(&l, 0, sizeof l);
    l.gouraud = (code & 0x10) != 0;
    l.semitrans = (code & 0x02) != 0;
    l.tpage = gpu_sw.texpage;
    int polyline = (code & 0x08) != 0;

    unpack_rgb(MEM_R32(addr), &l.r[0], &l.g[0], &l.b[0]);
    int n = 0, w = 1;
    while (n < 256) {
        if (n > 0 && l.gouraud) {
            if (w >= len) break;
            u32 c = MEM_R32(addr + 4u * (u32)w);
            if (polyline && n >= 2 && gp0_polyline_end(c)) break;
            unpack_rgb(c, &l.r[n], &l.g[n], &l.b[n]);
            w++;
        } else if (n > 0) {
            l.r[n] = l.r[0]; l.g[n] = l.g[0]; l.b[n] = l.b[0];
        }
        if (w >= len) break;
        u32 xy = MEM_R32(addr + 4u * (u32)w);
        if (polyline && !l.gouraud && n >= 2 && gp0_polyline_end(xy)) break;
        unpack_xy(xy, &l.x[n], &l.y[n]);
        w++;
        n++;
        if (!polyline && n == 2) break;
    }
    l.nverts = n;
    if (n < 2) return;
    gpu_sw_line(&l);
    ac3_capture_line(&l);
    if (gpu->line) gpu->line(&l);
}

void ac3_gpu_current_offset(s16 *x, s16 *y) {
    if (x) *x = (s16)gpu_sw.ofs_x;
    if (y) *y = (s16)gpu_sw.ofs_y;
}

static void gp0_env(u32 w) {
    u8 code = (u8)(w >> 24);
    gpu_sw_gp0_state(w);
    switch (code) {
    case 0xE1:
    case 0xE2: {
        u16 m, o;
        gpu_sw_texwin(&m, &o);
        if (gpu->set_draw_mode) gpu->set_draw_mode(gpu_sw.texpage, m, o);
        break;
    }
    case 0xE3:
    case 0xE4:
        if (code == 0xE4 && gpu->set_draw_area)
            gpu->set_draw_area((s16)gpu_sw.area_x0, (s16)gpu_sw.area_y0,
                               (s16)(gpu_sw.area_x1 + 1), (s16)(gpu_sw.area_y1 + 1));
        break;
    case 0xE5: {
        s16 ox = (s16)gpu_sw.ofs_x, oy = (s16)gpu_sw.ofs_y;
        {
            static s16 seen[8][2];
            static unsigned nseen;
            unsigned i = 0;
            for (; i < nseen; i++)
                if (seen[i][0] == ox && seen[i][1] == oy) break;
            if (i == nseen && nseen < 8) {
                seen[nseen][0] = ox; seen[nseen][1] = oy; nseen++;
                hle_warn_always("draw offset", "primitives now offset by (%d,%d)",
                                ox, oy);
            }
        }
        if (gpu->set_draw_offset) gpu->set_draw_offset(ox, oy);
        break;
    }
    case 0xE6:
        if (gpu->set_mask)
            gpu->set_mask(gpu_sw.mask_or != 0, gpu_sw.mask_and != 0);
        break;
    default:
        break;
    }
}

#define GP0_POS_X(v)   ((s16)((v) & 0x3FFu))
#define GP0_POS_Y(v)   ((s16)((v) & 0x1FFu))
#define GP0_COPY_W(v)  ((s16)(((((v) & 0x3FFu) - 1u) & 0x3FFu) + 1u))
#define GP0_COPY_H(v)  ((s16)(((((v) & 0x1FFu) - 1u) & 0x1FFu) + 1u))

static void gp0_fill(u32 c, u32 xy, u32 wh, const char *who) {
    PSX_RECT r;
    r.x = (s16)(xy & 0x3F0u);
    r.y = GP0_POS_Y(xy >> 16);
    r.w = (s16)(((wh & 0x3FFu) + 0x0Fu) & ~0x0Fu);
    r.h = (s16)((wh >> 16) & 0x1FFu);
    if (r.w <= 0 || r.h <= 0) return;

    u8 red, green, blue;
    unpack_rgb(c, &red, &green, &blue);
    ac3_vram_init();
    gpu_sw_fill(&r, (u16)(((blue >> 3) << 10) | ((green >> 3) << 5) | (red >> 3)));
    vram_mark_written(&r, VRAM_W_FILL, red, green, blue);
    ac3_oracle_vram(AC3_OVRAM_FILL, &r, 0, 0, 0);
    ac3_capture_fill_rect(&r, red, green, blue, who);
    if (gpu->fill_rect) gpu->fill_rect(&r, red, green, blue);
}

static void gp0_copy(u32 s, u32 d, u32 z) {
    PSX_RECT r;
    r.x = GP0_POS_X(s);
    r.y = GP0_POS_Y(s >> 16);
    r.w = GP0_COPY_W(z);
    r.h = GP0_COPY_H(z >> 16);
    s16 dx = GP0_POS_X(d), dy = GP0_POS_Y(d >> 16);

    ac3_vram_init();
    gpu_sw_copy(&r, dx, dy);
    PSX_RECT dst = { dx, dy, r.w, r.h };
    vram_mark_written(&dst, VRAM_W_COPY, 0, 0, 0);
    ac3_oracle_vram(AC3_OVRAM_COPY, &r, 0, dx, dy);
    ac3_capture_vram_copy(&r, dx, dy, 0);
    if (gpu->move_image) gpu->move_image(&r, dx, dy);
}

static void gp0_load_begin(u32 xy, u32 wh, PSX_RECT *out) {
    PSX_RECT r;
    r.x = GP0_POS_X(xy);
    r.y = GP0_POS_Y(xy >> 16);
    r.w = GP0_COPY_W(wh);
    r.h = GP0_COPY_H(wh >> 16);
    ac3_vram_init();
    gpu_sw_load_begin(&r);
    vram_mark_written(&r, VRAM_W_UPLOAD, 0, 0, 0);
    if (out) *out = r;
}

static void decode_load(u32 addr, int len) {
    PSX_RECT r;
    gp0_load_begin(MEM_R32(addr + 4), MEM_R32(addr + 8), &r);
    for (int i = 3; i < len; i++) gpu_sw_load_word(MEM_R32(addr + 4u * (u32)i));
    ac3_capture_load_image(&r, addr + 12u);
    ac3_oracle_vram(AC3_OVRAM_UPLOAD_OT, &r, addr + 12u, 0, 0);
    if (gpu->load_image) {
        u32 bytes = (len > 3) ? (u32)(len - 3) * 4u : 0u;
        const u16 *src = bytes ? (const u16 *)guest_ptr(addr + 12u, bytes) : NULL;
        if (src) gpu->load_image(&r, src);
    }
}

static int cmd_words(u32 addr, u8 code, int avail) {
    if (code >= 0x20 && code < 0x40) {
        int nv   = (code & 0x08) ? 4 : 3;
        int per  = (code & 0x04) ? 2 : 1;
        int gour = (code & 0x10) ? nv - 1 : 0;
        return 1 + nv * per + gour;
    }
    if (code >= 0x40 && code < 0x60) {
        if (!(code & 0x08)) return (code & 0x10) ? 4 : 3;
        for (int w = 1; w < avail; w++)
            if (MEM_R32(addr + 4u * (u32)w) == 0x55555555u) return w + 1;
        return avail;
    }
    if (code >= 0x60 && code < 0x80) {
        int n = 2 + ((code & 0x04) ? 1 : 0);
        if (((code >> 3) & 3) == 0) n++;
        return n;
    }
    if (code == 0x02) return 3;
    if (code >= 0x80 && code < 0xA0) return 4;
    if (code >= 0xA0 && code < 0xC0) {
        u32 size = MEM_R32(addr + 8);
        u32 w = (u32)GP0_COPY_W(size), h = (u32)GP0_COPY_H(size >> 16);
        u32 n = 3u + (w * h + 1u) / 2u;
        return (n > (u32)avail) ? avail : (int)n;
    }
    if (code >= 0xC0 && code < 0xE0) return 3;
    return 1;
}

static void decode_one(u32 addr, u8 code, int words) {
    if (code >= 0x20 && code < 0x40)      decode_poly(addr, code);
    else if (code >= 0x40 && code < 0x60) decode_line(addr, code, words);
    else if (code >= 0x60 && code < 0x80) decode_rect(addr, code);
    else if (code >= 0xE0)                gp0_env(MEM_R32(addr));
    else if (code >= 0xA0 && code < 0xC0) decode_load(addr, words);
    else if (code >= 0x80 && code < 0xA0)
        gp0_copy(MEM_R32(addr + 4), MEM_R32(addr + 8), MEM_R32(addr + 12));
    else if (code == 0x02)
        gp0_fill(MEM_R32(addr), MEM_R32(addr + 4), MEM_R32(addr + 8), "packet");
    else if (code != 0x00 && code != 0x01 && !(code >= 0xC0 && code < 0xE0))
        ac3_probe_unhandled_packet(addr, code, words);
}

static int gp0_in_polyline;

static void decode_packet(u32 addr, int len) {
    u32 a = addr;
    int left = len;
    while (left > 0) {
        u32 pending = gpu_sw_load_words_left();
        if (pending) {
            u32 take = (u32)left < pending ? (u32)left : pending;
            for (u32 i = 0; i < take; i++) gpu_sw_load_word(MEM_R32(a + 4u * i));
            a += 4u * take;
            left -= (int)take;
            continue;
        }
        if (gp0_in_polyline) {
            while (left > 0) {
                u32 w = MEM_R32(a);
                a += 4u; left--;
                if (gp0_polyline_end(w)) { gp0_in_polyline = 0; break; }
            }
            continue;
        }

        u8 code = (u8)(MEM_R32(a) >> 24);

        if (code >= 0x40 && code < 0x60 && (code & 0x08)) {
            int n = 1;
            while (n < left && !gp0_polyline_end(MEM_R32(a + 4u * (u32)n))) n++;
            int found = n < left;
            int take = found ? n + 1 : left;
            decode_one(a, code, take);
            if (!found) gp0_in_polyline = 1;
            a += 4u * (u32)take;
            left -= take;
            continue;
        }

        int n = cmd_words(a, code, left);
        if (n < 1) n = 1;
        if (n > left) n = left;
        decode_one(a, code, n);
        a += 4u * (u32)n;
        left -= n;
    }
}

static void walk_list(u32 head) {
    u32 cur = head & 0x1FFFFCu;
    unsigned guard = 0, nodes = 0, packets = 0;
    gpu_sw_load_cancel();
    gp0_in_polyline = 0;
#ifdef AC3_ENHANCED
    const gpu_sw_hooks_t *hk = gpu_sw_hooks;
    if (hk && hk->list) hk->list(1);
#endif
    while (guard++ < 0x40000u) {
        u32 tag = MEM_R32(cur);
        u32 next = OT_ADDR(tag);
        int len = (int)OT_LEN(tag);
        nodes++;
#ifdef AC3_ENHANCED
        if (hk && hk->node) hk->node(cur | 0x80000000u, len);
#endif
        if (len > 0) { decode_packet(cur + 4u, len); packets++; }
        if (next & 0x800000u) break;
        next &= 0x1FFFFCu;
        if (next == cur) break;
        cur = next;
    }
#ifdef AC3_ENHANCED
    if (hk && hk->list) hk->list(0);
#endif
    hle_trace("DrawOTag", "head=0x%08X nodes=%u packets=%u", head, nodes, packets);
    if (guard >= 0x40000u)
        hle_warn("DrawOTag", "ordering table did not terminate from 0x%08X", head);
}

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
