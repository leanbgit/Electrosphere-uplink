/*
 * hw_glue_ops.c -- primitives the glue holds back instead of drawing at once.
 *
 * Buffered (the sky and flare phases): the whole phase is kept, then sorted
 * out in flush_ops -- clouds and stars drawn as sprites are widened back to
 * their shape in widescreen, the game's sun and lens flare give way to the
 * sun effect, the horizon strip gives way to the far terrain, and a sky with
 * a roof over the map (no clouds or sun under it) is recognised.
 *
 * Deferred (see-through world primitives: smoke, glows, shadows): drawn after
 * the world effects (clouds, haze), in their original order, so they are not
 * covered by them.
 */
#include "hw_glue_internal.h"

/* ---- the sky and flare phases --------------------------------------------- */

enum { B_POLY, B_RECT, B_FILL, B_COPY };
typedef struct {
    u8 type;
    u8 mode;               /* polygons: 1 sprite, 2 strip, 3 ring quarter */
    u8 skip;               /* left out */
    float cx, cy;          /* the sprite's (or ring's) centre */
    gpu_sw_state_t st;
    gpu_poly_t poly;
    hw_pv_t pv[4];
    gpu_rect_t rect;
    PSX_RECT r;
    u16 pixel;
    s32 dx, dy;
} bop_t;
static bop_t *bops;
static int nbops, capbops;

#define ROOF_MIN 16        /* sky polygons above the camera that make a roof */
static u32 roof_sig;       /* the map that has one */

int buffering(void) {
    return G.flight && (G.phase == PH_SKY || G.phase == PH_FLARE);
}

static bop_t *bop_new(u8 type) {
    if (nbops == capbops) {
        int c = capbops ? capbops * 2 : 256;
        bop_t *q = realloc(bops, (size_t)c * sizeof *q);
        if (!q) return NULL;
        bops = q;
        capbops = c;
    }
    bop_t *b = &bops[nbops++];
    memset(b, 0, sizeof *b);
    b->type = type;
    return b;
}

int buffer_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]) {
    bop_t *b = bop_new(B_POLY);
    if (!b) return 0;
    b->st = *st;
    b->poly = *p;
    memcpy(b->pv, pv, sizeof b->pv);
    return 1;
}

int buffer_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    bop_t *b = bop_new(B_RECT);
    if (!b) return 0;
    b->st = *st;
    b->rect = *r;
    return 1;
}

int buffer_fill(const PSX_RECT *r, u16 pixel) {
    bop_t *b = bop_new(B_FILL);
    if (!b) return 0;
    b->r = *r;
    b->pixel = pixel;
    return 1;
}

int buffer_copy(const gpu_sw_state_t *st, const PSX_RECT *r, s32 dx, s32 dy) {
    bop_t *b = bop_new(B_COPY);
    if (!b) return 0;
    b->st = *st;
    b->r = *r;
    b->dx = dx;
    b->dy = dy;
    return 1;
}

int roofed(void) {
    return hw_map.valid && roof_sig == hw_map.signature;
}

static int poly_key_same(const gpu_poly_t *a, const gpu_poly_t *b) {
    return a->textured == b->textured && a->nverts == b->nverts && a->tpage == b->tpage &&
           a->clut == b->clut && a->semitrans == b->semitrans &&
           a->r[0] == b->r[0] && a->g[0] == b->g[0] && a->b[0] == b->b[0];
}

static int poly_has(const gpu_poly_t *q, s16 x, s16 y) {
    for (int k = 0; k < 4; k++) if (q->x[k] == x && q->y[k] == y) return 1;
    return 0;
}

/* the sky dome itself (not a cloud, star or glow) */
static int sky_geometry(const bop_t *b) {
    return b->type == B_POLY && b->poly.textured && !b->poly.semitrans;
}

/* sky dome above the camera everywhere */
static int sky_roof(const bop_t *b) {
    if (!sky_geometry(b)) return 0;
    const float *R = G.scene.rot;
    for (int k = 0; k < (b->poly.nverts == 4 ? 4 : 3); k++) {
        const hw_pv_t *v = &b->pv[k];
        if (!v->ok || R[1] * v->v.vx + R[4] * v->v.vy + R[7] * v->v.vz >= 0.0f) return 0;
    }
    return 1;
}

static void note_roof(void) {
    int roof = 0;
    for (int i = 0; i < nbops; i++) roof += sky_roof(&bops[i]);
    gstat.roof += roof;
    if (roof >= ROOF_MIN && !roofed()) {
        roof_sig = hw_map.signature;
        printf("enhanced: the sky has a roof over this map -- no clouds or sun under it\n");
    }
}

/* Where the sun is on screen (0: behind the camera). */
static int sun_on_screen(float *x, float *y) {
    float v[3];
    for (int i = 0; i < 3; i++)
        v[i] = G.scene.rot[3 * i] * G.scene.sun[0] + G.scene.rot[3 * i + 1] * G.scene.sun[1] +
               G.scene.rot[3 * i + 2] * G.scene.sun[2];
    if (!(v[2] > 0.02f)) return 0;
    *x = G.scene.H * G.scene.ws * v[0] / v[2];
    *y = G.scene.H * v[1] / v[2];
    return 1;
}

/* Textured quads of one look drawn in a row are sorted out: four quads around
 * one shared corner are a ring (a glow drawn in quarters), quads sharing
 * corners are a strip (the horizon band), any other quad is a sprite. */
static void classify_quads(int phase, int far_on) {
    for (int i = 0; i < nbops; ) {
        bop_t *b = &bops[i];
        if (b->type != B_POLY || !b->poly.textured || b->poly.nverts != 4 || sky_geometry(b)) {
            i++;
            continue;
        }
        int j = i + 1;
        while (j < nbops && bops[j].type == B_POLY && poly_key_same(&b->poly, &bops[j].poly)) j++;
        int quarter = -1;
        if (j - i == 4)
            for (int k = 0; k < 4 && quarter < 0; k++) {
                int all = 1;
                for (int q = i + 1; q < j; q++)
                    if (!poly_has(&bops[q].poly, b->poly.x[k], b->poly.y[k])) all = 0;
                if (all) quarter = k;
            }
        for (int q = i; q < j; q++) {
            bop_t *o = &bops[q];
            if (quarter >= 0) {
                gstat.rings++;
                o->mode = 3;
                o->cx = (float)b->poly.x[quarter];
                o->cy = (float)b->poly.y[quarter];
                continue;
            }
            int chained = 0;
            for (int r = i; r < j && !chained; r++) {
                if (r == q) continue;
                for (int k = 0; k < 4; k++)
                    if (poly_has(&bops[r].poly, o->poly.x[k], o->poly.y[k])) { chained = 1; break; }
            }
            if (chained) {
                o->mode = 2;
                gstat.strip++;
                if (phase == PH_SKY && far_on) { o->skip = 1; gstat.hidden++; }
            } else {
                gstat.sprites++;
                o->mode = 1;
                o->cx = o->cy = 0;
                for (int k = 0; k < 4; k++) { o->cx += o->poly.x[k] * 0.25f; o->cy += o->poly.y[k] * 0.25f; }
            }
        }
        i = j;
    }
}

/* With the sun effect on, the game's own sun (bright sprites and rings near
 * where the sun is) and its lens flare are left out. */
static void skip_game_sun(int phase, int sun_ahead, float sun_x, float sun_y, float ws) {
    for (int i = 0; i < nbops; i++) {
        bop_t *b = &bops[i];
        if (phase == PH_FLARE) {
            /* the game's lens flare is replaced by the sun effect -- but cutscenes
             * also draw their black letterbox bars in this phase (flat, opaque
             * rectangles). Skipping those let the sky show through the bars and
             * left the subtitles drawn over old frames. Only flare-like shapes
             * (textured or semi-transparent) are dropped. */
            int flat_opaque = (b->type == B_RECT && !b->rect.textured && !b->rect.semitrans) ||
                              (b->type == B_POLY && !b->poly.textured && !b->poly.semitrans);
            if ((b->type == B_POLY || b->type == B_RECT) && !flat_opaque) { b->skip = 1; gstat.sun++; }
            continue;
        }
        if (b->type != B_POLY || !sun_ahead || (b->mode != 1 && b->mode != 3)) continue;
        float dx = (b->cx - sun_x) / ws, dy = b->cy - sun_y;
        int bright = b->poly.r[0] >= 0x50 || b->poly.g[0] >= 0x50 || b->poly.b[0] >= 0x50;
        if (dx * dx + dy * dy < 28.0f * 28.0f && (b->mode == 3 || bright)) {
            b->skip = 1;
            gstat.sun++;
        }
    }
}

/* Draws the buffered phase. Sprites and rings were squeezed by the widescreen
 * projection like everything else; they are widened back around their centre. */
static void draw_buffered(float ws) {
    for (int i = 0; i < nbops; i++) {
        bop_t *b = &bops[i];
        switch (b->type) {
        case B_POLY:
            if (b->skip) break;
            if ((b->mode == 1 || b->mode == 3) && ws != 1.0f) {
                for (int k = 0; k < 4; k++) {
                    b->poly.x[k] = (s16)lrintf(b->cx + ((float)b->poly.x[k] - b->cx) * ws);
                    b->pv[k].ok = 0;
                }
            }
            hw_poly(&b->st, &b->poly, b->pv);
            break;
        case B_RECT: if (!b->skip) hw_rect(&b->st, &b->rect); break;
        case B_FILL: hw_fill(&b->r, b->pixel); break;
        case B_COPY: hw_copy(&b->st, &b->r, b->dx, b->dy); break;
        }
    }
}

void flush_ops(int phase) {
    float ws = G.scene.valid ? G.scene.ws : 1.0f;
    if (phase == PH_SKY && G.scene.valid && hw_map.valid) note_roof();
    int far_on = G.fx && (fx_flags_now() & FX_FAR) && hw_map.valid;
    int sun_on = G.fx && (fx_flags_now() & FX_SUN);
    float sun_x = 0, sun_y = 0;
    int sun_ahead = sun_on && G.scene.valid && sun_on_screen(&sun_x, &sun_y);
    classify_quads(phase, far_on);
    if (sun_on) skip_game_sun(phase, sun_ahead, sun_x, sun_y, ws);
    draw_buffered(ws);
    nbops = 0;
}

/* ---- see-through world primitives ----------------------------------------- */

enum { D_POLY, D_RECT, D_LINE, D_COPY };
typedef struct {
    u8 type;
    u32 order;             /* its place among the world's primitives */
    gpu_sw_state_t st;
    gpu_poly_t poly;
    hw_pv_t pv[4];
    gpu_rect_t rect;
    PSX_RECT r;
    s32 dx, dy;
    int line;              /* index into dlines (lines are large) */
} dop_t;
static dop_t *dops;
static int ndops, capdops;
static gpu_line_t *dlines;
static int ndlines, capdlines;

int deferring(void) {
    return G.flight && G.fx && G.begun && !G.ended && in_world() && hw_fx_ready();
}

static dop_t *dop_new(u8 type) {
    if (ndops == capdops) {
        int c = capdops ? capdops * 2 : 256;
        dop_t *q = realloc(dops, (size_t)c * sizeof *q);
        if (!q) return NULL;
        dops = q;
        capdops = c;
    }
    dop_t *d = &dops[ndops++];
    d->type = type;
    d->order = G.order;
    return d;
}

int defer_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]) {
    dop_t *d = dop_new(D_POLY);
    if (!d) return 0;
    d->st = *st;
    d->poly = *p;
    memcpy(d->pv, pv, sizeof d->pv);
    return 1;
}

int defer_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    dop_t *d = dop_new(D_RECT);
    if (!d) return 0;
    d->st = *st;
    d->rect = *r;
    return 1;
}

int defer_line(const gpu_sw_state_t *st, const gpu_line_t *l) {
    if (ndlines == capdlines) {
        int c = capdlines ? capdlines * 2 : 64;
        gpu_line_t *q = realloc(dlines, (size_t)c * sizeof *q);
        if (q) { dlines = q; capdlines = c; }
    }
    dop_t *d = ndlines < capdlines ? dop_new(D_LINE) : NULL;
    if (!d) return 0;
    d->st = *st;
    d->line = ndlines;
    dlines[ndlines++] = *l;
    return 1;
}

int defer_copy(const gpu_sw_state_t *st, const PSX_RECT *r, s32 dx, s32 dy) {
    dop_t *d = dop_new(D_COPY);
    if (!d) return 0;
    d->st = *st;
    d->r = *r;
    d->dx = dx;
    d->dy = dy;
    return 1;
}

void flush_deferred(void) {
    if (!ndops) return;
    hw_effect_depth_test(1);
    for (int i = 0; i < ndops; i++) {
        dop_t *d = &dops[i];
        hw_draw_order(d->order);
        switch (d->type) {
        case D_POLY: hw_poly(&d->st, &d->poly, d->pv); break;
        case D_RECT: hw_rect(&d->st, &d->rect); break;
        case D_LINE: hw_line(&d->st, &dlines[d->line]); break;
        case D_COPY: hw_copy(&d->st, &d->r, d->dx, d->dy); break;
        }
    }
    hw_effect_depth_test(0);
    hw_draw_order(0);
    gstat.deferred = ndops;
    ndops = ndlines = 0;
}

void discard_deferred(void) {
    ndops = ndlines = 0;
}

/* a new draw list: nothing held back yet */
void ops_reset(void) {
    nbops = 0;
    ndops = ndlines = 0;
}
