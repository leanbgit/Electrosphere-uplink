/*
 * gpu_packets.c -- the GPU's command words (GP0 packets) as the game writes
 * them into ordering tables: decoded into the software GPU's primitives
 * (gpu_sw.c), which draws them into VRAM and hands them to the enhanced
 * renderer's hooks. Also keeps, per 16x16 block of VRAM, what last wrote it
 * (for the warnings and the debugging tools).
 */
#include "gpu_internal.h"
#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include "ac3_oracle.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* ---- what last wrote each VRAM block -------------------------------------- */

#define VRAM_BLK      16
#define VRAM_BLK_W    (GPU_VRAM_W / VRAM_BLK)
#define VRAM_BLK_H    (GPU_VRAM_H / VRAM_BLK)

extern u32 ac3_vblank_count(void);
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

void vram_mark_written(const PSX_RECT *r, u8 kind,
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


/* ---- primitives ----------------------------------------------------------- */

/* a palette that is all zero draws nothing: worth a warning */
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

void draw_rect(gpu_rect_t *r) {
    r->tpage = gpu_sw.texpage;
    /* co-op's second video memory bank (gpu_sw.c): a sprite has no texture
     * page of its own, so its colour table's bank bit picks the bank for both */
    if (r->textured && (r->clut & 0x8000u) && ac3_vram_bank) r->tpage |= 0x800u;
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


/* ---- drawing environment, fills, copies, uploads -------------------------- */

void ac3_gpu_current_offset(s16 *x, s16 *y) {
    if (x) *x = (s16)gpu_sw.ofs_x;
    if (y) *y = (s16)gpu_sw.ofs_y;
}

void gp0_env(u32 w) {
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

void gp0_fill(u32 c, u32 xy, u32 wh, const char *who) {
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

void gp0_copy(u32 s, u32 d, u32 z) {
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

void gp0_load_begin(u32 xy, u32 wh, PSX_RECT *out) {
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


/* ---- packets and ordering tables ------------------------------------------ */

/* the words a command takes (0: unknown) */
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

int gp0_in_polyline;

void decode_packet(u32 addr, int len) {
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

void walk_list(u32 head) {
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
