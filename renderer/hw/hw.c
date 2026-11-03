/*
 * hw.c -- the enhanced renderer's drawing, CPU side (all builds).
 *
 * The game's primitives arrive here from the glue (hw_glue.c) and become a
 * command list (H.cmds, with vertices in H.verts): draws batched by pipeline
 * and scissor, fills, uploads, and the syncs needed where a primitive reads
 * VRAM that earlier ones wrote (textures and palettes). The GPU side records
 * the list: hw_vk_frame.c (native, Vulkan) or, on the web, the list is sent
 * to the render worker (renderer/webgpu/hw_packet.c, hw_webgpu.c). Device
 * set-up is in hw_vk_setup.c / hw_webgpu.c.
 */
#include "hw_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hw_state_t hw_state;
#define H hw_state

static inline s32 sx11(s32 v) { return (s32)((u32)v << 21) >> 21; }

int hw_scale(void) { return H.S; }

void hw_set_perspective(int on) { H.persp = on ? 1 : 0; }

void hw_frame_stats(u32 *prims, u32 *precise) {
    if (prims) *prims = H.last_prims;
    if (precise) *precise = H.last_precise;
}

static void dirty_add(rect_t r);

static void *grow(void *p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap) return p;
    u32 n = *cap ? *cap : 1024;
    while (n < need) n *= 2;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) return p;
    *cap = n;
    return q;
}

static cmd_t *new_cmd(u8 op) {
    H.cmds = grow(H.cmds, &H.cap_cmds, H.ncmds + 1, sizeof(cmd_t));
    if (H.ncmds >= H.cap_cmds) return NULL;
    cmd_t *c = &H.cmds[H.ncmds++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

cmd_t *hw_new_cmd(u8 op) { return new_cmd(op); }

void hw_mark_dirty(s32 x0, s32 y0, s32 x1, s32 y1) {
    rect_t r = { x0, y0, x1, y1 };
    dirty_add(r);
}

static u32 push_verts(const hw_vtx_t *v, u32 n) {
    H.verts = grow(H.verts, &H.cap_verts, H.nverts + n, sizeof(hw_vtx_t));
    if (H.nverts + n > H.cap_verts) return UINT32_MAX;
    u32 first = H.nverts;
    memcpy(&H.verts[first], v, n * sizeof(hw_vtx_t));
    H.nverts += n;
    return first;
}

static int same_rect(const VkRect2D *a, const VkRect2D *b) {
    return a->offset.x == b->offset.x && a->offset.y == b->offset.y &&
           a->extent.width == b->extent.width && a->extent.height == b->extent.height;
}

static void add_draw(u8 pipe, u8 pass, VkRect2D sc, u32 first, u32 count) {
    if (H.ncmds) {
        cmd_t *l = &H.cmds[H.ncmds - 1];
        if (l->op == OP_DRAW && l->pipe == pipe && l->pass == pass &&
            same_rect(&l->sc, &sc) && l->first + l->count == first) {
            l->count += count;
            return;
        }
    }
    cmd_t *c = new_cmd(OP_DRAW);
    if (!c) return;
    c->pipe = pipe; c->pass = pass; c->sc = sc; c->first = first; c->count = count;
}

static void add_draw_pair(u8 pipe1, u8 pipe2, VkRect2D sc, u32 first, u32 count) {
    if (H.ncmds >= 2) {
        cmd_t *a = &H.cmds[H.ncmds - 2], *b = &H.cmds[H.ncmds - 1];
        if (a->op == OP_DRAW && b->op == OP_DRAW &&
            a->pipe == pipe1 && a->pass == 1 && b->pipe == pipe2 && b->pass == 2 &&
            same_rect(&a->sc, &sc) && same_rect(&b->sc, &sc) &&
            a->first == b->first && a->count == b->count &&
            a->first + a->count == first) {
            a->count += count;
            b->count += count;
            return;
        }
    }
    add_draw(pipe1, 1, sc, first, count);
    cmd_t *c = new_cmd(OP_DRAW);
    if (!c) return;
    c->pipe = pipe2; c->pass = 2; c->sc = sc; c->first = first; c->count = count;
}

static int split_wrap(s32 x, s32 y, s32 w, s32 h, rect_t *out) {
    int n = 0;
    if (w <= 0 || h <= 0) return 0;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    s32 xs[2] = { x, 0 }, ws[2] = { w, 0 };
    if (x + w > VRAM_W) { ws[0] = VRAM_W - x; ws[1] = w - ws[0]; }
    s32 ys[2] = { y, 0 }, hs[2] = { h, 0 };
    if (y + h > VRAM_H) { hs[0] = VRAM_H - y; hs[1] = h - hs[0]; }
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
            if (ws[i] > 0 && hs[j] > 0) {
                out[n].x0 = xs[i]; out[n].y0 = ys[j];
                out[n].x1 = xs[i] + ws[i]; out[n].y1 = ys[j] + hs[j];
                n++;
            }
    return n;
}

static int overlap(const rect_t *a, const rect_t *b) {
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

static void dirty_add(rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > VRAM_W) r.x1 = VRAM_W;
    if (r.y1 > VRAM_H) r.y1 = VRAM_H;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    for (int i = 0; i < H.ndirty; ) {
        rect_t *d = &H.dirty[i];
        if (overlap(d, &r)) {
            if (d->x0 < r.x0) r.x0 = d->x0;
            if (d->y0 < r.y0) r.y0 = d->y0;
            if (d->x1 > r.x1) r.x1 = d->x1;
            if (d->y1 > r.y1) r.y1 = d->y1;
            H.dirty[i] = H.dirty[--H.ndirty];
            i = 0;
            continue;
        }
        i++;
    }
    if (H.ndirty == MAX_DIRTY) {
        for (int i = 0; i < H.ndirty; i++) {
            if (H.dirty[i].x0 < r.x0) r.x0 = H.dirty[i].x0;
            if (H.dirty[i].y0 < r.y0) r.y0 = H.dirty[i].y0;
            if (H.dirty[i].x1 > r.x1) r.x1 = H.dirty[i].x1;
            if (H.dirty[i].y1 > r.y1) r.y1 = H.dirty[i].y1;
        }
        H.ndirty = 0;
    }
    H.dirty[H.ndirty++] = r;
}

static void sync_for(const rect_t *fp, int n) {
    for (int i = 0; i < H.ndirty; ) {
        int hit = 0;
        for (int j = 0; j < n; j++)
            if (overlap(&H.dirty[i], &fp[j])) { hit = 1; break; }
        if (!hit) { i++; continue; }
        cmd_t *c = new_cmd(OP_SYNC);
        if (!c) return;
        c->x = (s16)H.dirty[i].x0; c->y = (s16)H.dirty[i].y0;
        c->w = (s16)(H.dirty[i].x1 - H.dirty[i].x0);
        c->h = (s16)(H.dirty[i].y1 - H.dirty[i].y0);
        H.dirty[i] = H.dirty[--H.ndirty];
    }
}

static int tex_footprint(u16 tpage, u16 clut, rect_t *fp) {
    s32 px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256;
    u32 depth = (tpage >> 7) & 3u;
    if (depth == 3) depth = 2;
    s32 w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    int n = split_wrap(px, py, w, 256, fp);
    if (depth < 2)
        n += split_wrap((clut & 63) * 16, (clut >> 6) & 511, depth == 0 ? 16 : 256, 1, fp + n);
    return n;
}

static int scissor_of(const gpu_sw_state_t *st, VkRect2D *sc) {
    s32 x0 = st->area_x0, y0 = st->area_y0, x1 = st->area_x1, y1 = st->area_y1;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1;
    if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x1 < x0 || y1 < y0) return 0;
    sc->offset.x = x0 * H.S;
    sc->offset.y = y0 * H.S;
    sc->extent.width = (u32)(x1 - x0 + 1) * (u32)H.S;
    sc->extent.height = (u32)(y1 - y0 + 1) * (u32)H.S;
    return 1;
}

static void twin_word(u16 tw_mask, u16 tw_off, u32 *out) {
    u32 mu = tw_mask & 0xFFu, mv = tw_mask >> 8;
    u32 ou = tw_off & 0xFFu,  ov = tw_off >> 8;
    *out = (~mu & 0xFFu) | ((ou & mu) << 8) | ((~mv & 0xFFu) << 16) | ((ov & mv) << 24);
}

static void set_vtx(hw_vtx_t *o, float x, float y, float z, int persp,
                    float u, float v, u8 r, u8 g, u8 b, const u32 info[4]) {
    o->x = x * (float)H.S;
    o->y = y * (float)H.S;
    o->z = z;
    o->w = persp ? 1.0f : 0.0f;
    o->u = u; o->v = v;
    o->r = r; o->g = g; o->b = b; o->a = 255;
    memcpy(o->info, info, sizeof o->info);
}

static void emit(const gpu_sw_state_t *st, int textured, int semitrans, u16 tpage,
                 u16 clut, const hw_vtx_t *v, u32 n, rect_t bbox) {
    VkRect2D sc;
    if (!n || !scissor_of(st, &sc)) return;
    rect_t area = { st->area_x0, st->area_y0, st->area_x1 + 1, st->area_y1 + 1 };
    if (bbox.x0 < area.x0) bbox.x0 = area.x0;
    if (bbox.y0 < area.y0) bbox.y0 = area.y0;
    if (bbox.x1 > area.x1) bbox.x1 = area.x1;
    if (bbox.y1 > area.y1) bbox.y1 = area.y1;
    if (textured) {
        rect_t fp[8];
        sync_for(fp, tex_footprint(tpage, clut, fp));
    }
    if (st->mask_and) sync_for(&bbox, 1);
    u32 first = push_verts(v, n);
    if (first == UINT32_MAX) return;
    u8 blend = (u8)(P_BLEND0 + ((tpage >> 5) & 3));
    if (!semitrans)      add_draw(P_OPAQUE, 0, sc, first, n);
    else if (!textured)  add_draw(blend, 0, sc, first, n);
    else                 add_draw_pair(P_OPAQUE, blend, sc, first, n);
    dirty_add(bbox);
}

static void bbox_add(rect_t *b, float x, float y) {
    s32 x0 = (s32)x - 1, y0 = (s32)y - 1, x1 = (s32)x + 2, y1 = (s32)y + 2;
    if (x0 < b->x0) b->x0 = x0;
    if (y0 < b->y0) b->y0 = y0;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}

static void prim_info(u32 info[4], int textured, int raw, int flat, u16 tpage,
                      u16 clut, u16 tw_mask, u16 tw_off, const gpu_sw_state_t *st) {
    info[0] = tpage;
    info[1] = clut;
    twin_word(tw_mask, tw_off, &info[2]);
    info[3] = (textured ? F_TEXTURED : 0u) | (raw ? F_RAW : 0u) |
              (st->mask_or ? F_MASK_OR : 0u) | (flat ? F_FLAT : 0u) |
              (st->mask_and ? F_CHECK : 0u) | (H.ztest ? F_ZTEST : 0u) | (H.draw_order << 7);
}

void hw_effect_depth_test(int on) { H.ztest = on; }
void hw_draw_order(u32 order) { H.draw_order = order; }

void hw_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]) {
    if (!H.ready) return;
    int n = p->nverts == 4 ? 4 : 3;
    int textured = p->textured != 0;
    int raw = textured && p->raw;
    u32 info[4];
    prim_info(info, textured, raw, !p->gouraud, p->tpage, p->clut, p->twin_mask,
              p->twin_off, st);
    if (textured) {
        /* The console never samples a polygon's largest u or v (its right and
         * bottom edges are left out), but at a higher resolution the edge
         * pixels can land on it and pick up the neighbouring texture: thin
         * seams between tiles (the Geofront ceiling). Clamp to the texels the
         * console would use; the shader reads the limits from the upper
         * halves of info[0] and info[1] (bit 15 of info[0] marks them). */
        u32 umin = p->u[0], umax = p->u[0], vmin = p->v[0], vmax = p->v[0];
        for (int i = 1; i < n; i++) {
            if (p->u[i] < umin) umin = p->u[i];
            if (p->u[i] > umax) umax = p->u[i];
            if (p->v[i] < vmin) vmin = p->v[i];
            if (p->v[i] > vmax) vmax = p->v[i];
        }
        if (umax > umin) umax--;
        if (vmax > vmin) vmax--;
        info[0] = (info[0] & 0x7FFFu) | 0x8000u | (umin << 16) | (umax << 24);
        info[1] = (info[1] & 0xFFFFu) | (vmin << 16) | (vmax << 24);
    }

    s32 ix[4], iy[4];
    float fx[4], fy[4], fz[4];
    int ok[4];
    for (int i = 0; i < n; i++) {
        ix[i] = sx11(p->x[i]) + st->ofs_x;
        iy[i] = sx11(p->y[i]) + st->ofs_y;
        ok[i] = pv && pv[i].ok;
        if (ok[i]) {
            fx[i] = pv[i].v.sx + (float)st->ofs_x;
            fy[i] = pv[i].v.sy + (float)st->ofs_y;
            fz[i] = pv[i].v.vz;
        } else {
            fx[i] = (float)ix[i];
            fy[i] = (float)iy[i];
            fz[i] = 0.0f;
        }
    }

    hw_vtx_t v[6];
    u32 nv = 0;
    rect_t bb = { 1 << 20, 1 << 20, -(1 << 20), -(1 << 20) };
    static const int tri[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    for (int t = 0; t < (n == 4 ? 2 : 1); t++) {
        const int *k = tri[t];
        s32 minx = ix[k[0]], maxx = minx, miny = iy[k[0]], maxy = miny;
        for (int j = 1; j < 3; j++) {
            if (ix[k[j]] < minx) minx = ix[k[j]];
            if (ix[k[j]] > maxx) maxx = ix[k[j]];
            if (iy[k[j]] < miny) miny = iy[k[j]];
            if (iy[k[j]] > maxy) maxy = iy[k[j]];
        }
        if (miny == maxy || maxy - miny >= 512 || maxx - minx >= 1024) continue;
        s32 area = (ix[k[1]] - ix[k[0]]) * (iy[k[2]] - iy[k[1]]) -
                   (ix[k[2]] - ix[k[1]]) * (iy[k[1]] - iy[k[0]]);
        if (!area) continue;
        int precise = ok[k[0]] && ok[k[1]] && ok[k[2]];
        float px[3], py[3];
        for (int j = 0; j < 3; j++) {
            px[j] = precise ? fx[k[j]] : (float)ix[k[j]];
            py[j] = precise ? fy[k[j]] : (float)iy[k[j]];
        }
        float du = 0.0f, dv = 0.0f;
        if (textured) {
            float ex1 = px[1] - px[0], ey1 = py[1] - py[0];
            float ex2 = px[2] - px[0], ey2 = py[2] - py[0];
            float det = ex1 * ey2 - ex2 * ey1;
            if (det != 0.0f) {
                float u1 = (float)p->u[k[1]] - p->u[k[0]], u2 = (float)p->u[k[2]] - p->u[k[0]];
                float v1 = (float)p->v[k[1]] - p->v[k[0]], v2 = (float)p->v[k[2]] - p->v[k[0]];
                float ux = (u1 * ey2 - u2 * ey1) / det, uy = (ex1 * u2 - ex2 * u1) / det;
                float vx = (v1 * ey2 - v2 * ey1) / det, vy = (ex1 * v2 - ex2 * v1) / det;
                du = 0.5f - 0.5f * (ux + uy);
                dv = 0.5f - 0.5f * (vx + vy);
            }
        }
        for (int j = 0; j < 3; j++) {
            int i = k[j];
            set_vtx(&v[nv++], px[j], py[j], precise ? fz[i] : 0.0f,
                    precise && H.persp, (float)p->u[i] + du, (float)p->v[i] + dv,
                    p->r[i], p->g[i], p->b[i], info);
            bbox_add(&bb, px[j], py[j]);
        }
        H.prims++;
        if (precise) H.precise++;
    }
    emit(st, textured, p->semitrans, p->tpage, p->clut, v, nv, bb);
}

void hw_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    if (!H.ready) return;
    int textured = r->textured != 0;
    s32 x = sx11(sx11(r->x) + st->ofs_x);
    s32 y = sx11(sx11(r->y) + st->ofs_y);
    s32 w = r->w & 0x3FF, h = r->h & 0x1FF;
    if (!w || !h) return;
    u32 info[4];
    prim_info(info, textured, textured && r->raw, 1, r->tpage, r->clut,
              r->twin_mask, r->twin_off, st);

    float u0 = r->u, u1 = (float)r->u + (float)w;
    float v0 = r->v, v1 = (float)r->v + (float)h;
    if (textured && (st->texpage & 0x1000u)) {
        u0 = (float)(r->u | 1) + 1.0f; u1 = u0 - (float)w;
    }
    if (textured && (st->texpage & 0x2000u)) {
        v0 = (float)r->v + 1.0f; v1 = v0 - (float)h;
    }
    hw_vtx_t v[6];
    float X0 = (float)x, Y0 = (float)y, X1 = (float)(x + w), Y1 = (float)(y + h);
    set_vtx(&v[0], X0, Y0, 0, 0, u0, v0, r->r, r->g, r->b, info);
    set_vtx(&v[1], X1, Y0, 0, 0, u1, v0, r->r, r->g, r->b, info);
    set_vtx(&v[2], X0, Y1, 0, 0, u0, v1, r->r, r->g, r->b, info);
    v[3] = v[1];
    v[4] = v[2];
    set_vtx(&v[5], X1, Y1, 0, 0, u1, v1, r->r, r->g, r->b, info);
    rect_t bb = { x, y, x + w, y + h };
    H.prims++;
    emit(st, textured, r->semitrans, r->tpage, r->clut, v, 6, bb);
}

void hw_line(const gpu_sw_state_t *st, const gpu_line_t *l) {
    if (!H.ready || l->nverts < 2) return;
    u32 info[4];
    prim_info(info, 0, 0, !l->gouraud, l->tpage, 0, 0, 0, st);
    for (int i = 0; i + 1 < l->nverts; i++) {
        s32 x0 = sx11(l->x[i]) + st->ofs_x, y0 = sx11(l->y[i]) + st->ofs_y;
        s32 x1 = sx11(l->x[i + 1]) + st->ofs_x, y1 = sx11(l->y[i + 1]) + st->ofs_y;
        s32 dx = abs(x1 - x0), dy = abs(y1 - y0);
        if (dx >= 1024 || dy >= 512) continue;
        u8 r0 = l->r[i], g0 = l->g[i], b0 = l->b[i];
        u8 r1 = l->gouraud ? l->r[i + 1] : r0;
        u8 g1 = l->gouraud ? l->g[i + 1] : g0;
        u8 b1 = l->gouraud ? l->b[i + 1] : b0;
        float ax, ay, bx, by, cx, cy, ex, ey;
        if (dx >= dy) {
            if (x0 > x1) { s32 t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
                           u8 c = r0; r0 = r1; r1 = c; c = g0; g0 = g1; g1 = c;
                           c = b0; b0 = b1; b1 = c; }
            ax = (float)x0;       ay = (float)y0;
            bx = (float)x1 + 1;   by = (float)y1;
            cx = (float)x0;       cy = (float)y0 + 1;
            ex = (float)x1 + 1;   ey = (float)y1 + 1;
        } else {
            if (y0 > y1) { s32 t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
                           u8 c = r0; r0 = r1; r1 = c; c = g0; g0 = g1; g1 = c;
                           c = b0; b0 = b1; b1 = c; }
            ax = (float)x0;       ay = (float)y0;
            bx = (float)x0 + 1;   by = (float)y0;
            cx = (float)x1;       cy = (float)y1 + 1;
            ex = (float)x1 + 1;   ey = (float)y1 + 1;
        }
        hw_vtx_t v[6];
        set_vtx(&v[0], ax, ay, 0, 0, 0, 0, r0, g0, b0, info);
        set_vtx(&v[1], bx, by, 0, 0, 0, 0, dx >= dy ? r1 : r0, dx >= dy ? g1 : g0, dx >= dy ? b1 : b0, info);
        set_vtx(&v[2], cx, cy, 0, 0, 0, 0, dx >= dy ? r0 : r1, dx >= dy ? g0 : g1, dx >= dy ? b0 : b1, info);
        v[3] = v[1];
        v[4] = v[2];
        set_vtx(&v[5], ex, ey, 0, 0, 0, 0, r1, g1, b1, info);
        rect_t bb = { (x0 < x1 ? x0 : x1), (y0 < y1 ? y0 : y1),
                      (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1 };
        H.prims++;
        emit(st, 0, l->semitrans, l->tpage, 0, v, 6, bb);
    }
}

static float c5(u16 p, int shift) {
    return (float)((p >> shift) & 31u) / 31.0f;
}

void hw_fill(const PSX_RECT *r, u16 pixel) {
    if (!H.ready || !r) return;
    rect_t pieces[4];
    int n = split_wrap(r->x, r->y, r->w, r->h, pieces);
    for (int i = 0; i < n; i++) {
        cmd_t *c = new_cmd(OP_FILL);
        if (!c) return;
        c->x = (s16)pieces[i].x0; c->y = (s16)pieces[i].y0;
        c->w = (s16)(pieces[i].x1 - pieces[i].x0);
        c->h = (s16)(pieces[i].y1 - pieces[i].y0);
        c->color[0] = c5(pixel, 0);
        c->color[1] = c5(pixel, 5);
        c->color[2] = c5(pixel, 10);
        c->color[3] = (pixel & 0x8000u) ? 1.0f : 0.0f;
        dirty_add(pieces[i]);
    }
}

void hw_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy) {
    if (!H.ready || !src || src->w <= 0 || src->h <= 0) return;
    rect_t fp[4];
    sync_for(fp, split_wrap(src->x, src->y, src->w, src->h, fp));

    u32 info[4] = { 0, 0, 0, F_COPY | (st->mask_or ? F_MASK_OR : 0u) |
                                   (st->mask_and ? F_CHECK : 0u) };
    VkRect2D full = { { 0, 0 }, { H.W, H.Hs } };
    u8 pipe = P_COPY;
    rect_t d[4];
    int n = split_wrap(dx, dy, src->w, src->h, d);
    s32 bx = dx & (VRAM_W - 1), by = dy & (VRAM_H - 1);
    for (int i = 0; i < n; i++) {
        s32 ox = (d[i].x0 - bx + VRAM_W) & (VRAM_W - 1);
        s32 oy = (d[i].y0 - by + VRAM_H) & (VRAM_H - 1);
        float su = (float)((src->x + ox) * H.S), sv = (float)((src->y + oy) * H.S);
        float w = (float)(d[i].x1 - d[i].x0), h = (float)(d[i].y1 - d[i].y0);
        float S = (float)H.S;
        hw_vtx_t v[6];
        set_vtx(&v[0], (float)d[i].x0, (float)d[i].y0, 0, 0, su, sv, 0, 0, 0, info);
        set_vtx(&v[1], (float)d[i].x1, (float)d[i].y0, 0, 0, su + w * S, sv, 0, 0, 0, info);
        set_vtx(&v[2], (float)d[i].x0, (float)d[i].y1, 0, 0, su, sv + h * S, 0, 0, 0, info);
        v[3] = v[1];
        v[4] = v[2];
        set_vtx(&v[5], (float)d[i].x1, (float)d[i].y1, 0, 0, su + w * S, sv + h * S, 0, 0, 0, info);
        if (st->mask_and) sync_for(&d[i], 1);
        u32 first = push_verts(v, 6);
        if (first == UINT32_MAX) return;
        add_draw(pipe, 0, full, first, 6);
        dirty_add(d[i]);
    }
}

void hw_upload(const PSX_RECT *r, const u16 *vram) {
    if (!H.ready || !r || !vram) return;
    rect_t pieces[4];
    int n = split_wrap(r->x, r->y, r->w, r->h, pieces);
    for (int i = 0; i < n; i++) {
        u32 w = (u32)(pieces[i].x1 - pieces[i].x0), h = (u32)(pieces[i].y1 - pieces[i].y0);
        u32 need = H.nstaging + w * h * 4u;
        H.staging = grow(H.staging, &H.cap_staging, need, 1);
        if (need > H.cap_staging) return;
        u8 *o = H.staging + H.nstaging;
        for (u32 yy = 0; yy < h; yy++)
            for (u32 xx = 0; xx < w; xx++) {
                u16 p = vram[(u32)(pieces[i].y0 + (s32)yy) * VRAM_W + (u32)(pieces[i].x0 + (s32)xx)];
                u32 r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
                o[0] = (u8)((r5 << 3) | (r5 >> 2));
                o[1] = (u8)((g5 << 3) | (g5 >> 2));
                o[2] = (u8)((b5 << 3) | (b5 >> 2));
                o[3] = (p & 0x8000u) ? 255 : 0;
                o += 4;
            }
        cmd_t *c = new_cmd(OP_UPLOAD);
        if (!c) return;
        c->x = (s16)pieces[i].x0; c->y = (s16)pieces[i].y0;
        c->w = (s16)w; c->h = (s16)h;
        c->staging = H.nstaging;
        H.nstaging = need;
        cmd_t *z = new_cmd(OP_ZCLEAR);
        if (!z) return;
        z->x = c->x; z->y = c->y; z->w = c->w; z->h = c->h;
    }
}
