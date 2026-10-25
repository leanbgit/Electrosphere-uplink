#include "hw_glue.h"
#include "hw_world.h"
#include "gpu_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

extern int ac3_write_png_rgb(const char *path, unsigned w, unsigned h,
                             const unsigned char *rgb);

#define GAME_GP 0x800BCB74u

static inline s32 sx11(s32 v) { return (s32)((u32)v << 21) >> 21; }

#define REC_MAGIC   0x52334341u
#define REC_VERSION 2u
#define REC_FIELDS  4

enum { R_POLY = 1, R_RECT, R_LINE, R_FILL, R_COPY, R_UPLOAD, R_FIELD, R_GTE,
       R_LIST, R_PHASE, R_RAM };

static struct {
    int armed, active, fields_left;
    u8 *buf;
    size_t len, cap;
} rec;

static void rec_put(const void *p, size_t n) {
    if (rec.len + n > rec.cap) {
        size_t c = rec.cap ? rec.cap : (1u << 22);
        while (c < rec.len + n) c *= 2;
        u8 *q = realloc(rec.buf, c);
        if (!q) { rec.active = 0; return; }
        rec.buf = q;
        rec.cap = c;
    }
    memcpy(rec.buf + rec.len, p, n);
    rec.len += n;
}

static void rec_head(u8 type, u32 len) {
    rec_put(&type, 1);
    rec_put(&len, 4);
}

void hw_rec_arm(void) {
    if (rec.active || rec.armed) return;
    rec.armed = 1;
    printf("capture: recording the next %d fields\n", REC_FIELDS);
}

static void rec_gte(u32 key, const ac3_pgxp_vtx_t *v) {
    if (!rec.active) return;
    rec_head(R_GTE, 4 + sizeof *v);
    rec_put(&key, 4);
    rec_put(v, sizeof *v);
}

static int in_flight(void);

static void rec_start(void) {
    rec.len = 0;
    rec.active = 1;
    rec.armed = 0;
    rec.fields_left = REC_FIELDS;
    ac3_pgxp_tap = rec_gte;
    u32 hdr[4] = { REC_MAGIC, REC_VERSION, REC_FIELDS, (u32)ac3_hd.widescreen };
    rec_put(hdr, sizeof hdr);
    rec_put(ac3_vram, (size_t)GPU_VRAM_W * GPU_VRAM_H * 2);
    if (in_flight()) {
        rec_head(R_RAM, AC3_RAM_SIZE);
        rec_put(ac3_ram, AC3_RAM_SIZE);
    }
}

static void rec_finish(void) {
    rec.active = 0;
    ac3_pgxp_tap = NULL;
    MKDIR("captures_hd");
    char path[260];
    for (int i = 0; i < 10000; i++) {
        snprintf(path, sizeof path, "captures_hd/frame_%04d.ac3r", i);
        FILE *t = fopen(path, "rb");
        if (!t) break;
        fclose(t);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "capture: cannot write %s\n", path); return; }
    fwrite(rec.buf, 1, rec.len, f);
    fclose(f);
    printf("capture: %s (%.1f MB)\n", path, (double)rec.len / 1048576.0);
}

static int in_flight(void) {
    u32 g = MEM_R32(GAME_GP + 748);
    if ((g & 0x1FFFFFu) == 0 || (g >> 24) != 0x80u) return 0;
    return (int)MEM_R32(g + 44) == 2;
}

enum { PH_NONE, PH_SKY, PH_TERRAIN, PH_OBJECTS, PH_FLARE, PH_HUD };

static struct {
    int flight;
    int fx;
    int phase;
    int begun, ended;
    hw_scene_t scene;
    u32 ot[2];
    u32 fields;
    u32 order;
    int band_copies;
    int backdrop;          /* widescreen backdrop already drawn this frame */
    float fog[3];
} G = { .fog = { 0.55f, 0.6f, 0.6f } };

typedef struct { PSX_RECT src; s32 dx, dy; int n; } band_t;
#define MAX_BANDS 32
static band_t band_cur[MAX_BANDS], band_near[MAX_BANDS];
static int nband_cur, nband_near;

/* Fog bands. The game fades its colour tables (CLUTs) with distance: while it
 * draws the ground, it copies a far-to-near series of pre-faded tables from
 * spare VRAM into the live table area (16x16 blocks from rows 240/496, 16x64
 * columns from y 384/448, all into 320,256). With atmosphere on, the enhanced
 * renderer does its own haze, so each copy of a series is replaced by the
 * series' nearest (last) table. A series is the same destination and size.
 *
 * Copies that read the picture itself are not fog bands: the afterburner's heat
 * haze copies 16x1 strips of the screen into a CLUT (352,299) and draws a
 * see-through sprite with it, once per puff. Treated as a band, every puff got
 * the previous frame's last strip, so the plume showed blocky rectangles in
 * whatever colour that spot had (green when it was the HUD). */
/* Original fog (ac3_hd.game_fog): the game's own fade is kept on the ground it
 * draws (no band replacement), and the far terrain gets the same fade. Its
 * distances are measured from the game: ground drawn with the band table
 * before the last band copy is faded; the second-to-last band starts the fade
 * and the farthest faded ground ends it (about 15,000-19,000 to 30,000 units,
 * depending on the mission). */
static struct {
    s32 dx, dy, w, h;      /* the main band's destination (0 size: none) */
    int k, n_prev;         /* band copies so far this frame; last frame's total */
    float lo, hi;          /* this frame's fade start and end samples */
    float fs, fe;          /* smoothed */
    PSX_RECT first, last;  /* this frame's farthest and nearest table */
    float col[3];          /* the colour the game fades to */
    int have, have_col;
} FOG = { .lo = 1e30f };

/* The colour the game fades to: each entry of the farthest table is
 * near + a * (colour - near). Least squares over the entries for a and
 * b = a * colour (4 unknowns). */
static void fog_colour(void) {
    const PSX_RECT *f = &FOG.first, *n = &FOG.last;
    double A[4][5] = { { 0 } };
    int used = 0;
    for (int y = 0; y < n->h; y++)
        for (int x = 0; x < n->w; x++) {
            u16 cn = ac3_vram[((n->y + y) & 511) * GPU_VRAM_W + ((n->x + x) & 1023)];
            u16 cf = ac3_vram[((f->y + y) & 511) * GPU_VRAM_W + ((f->x + x) & 1023)];
            if (!cn || !cf) continue;
            for (int c = 0; c < 3; c++) {
                double nv = (double)((cn >> (5 * c)) & 31) / 31.0, fv = (double)((cf >> (5 * c)) & 31) / 31.0;
                double row[5] = { -nv, c == 0, c == 1, c == 2, fv - nv };
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 5; j++) A[i][j] += row[i] * row[j];
            }
            used++;
        }
    if (used < 16) return;
    for (int i = 0; i < 4; i++) {                 /* Gauss-Jordan */
        int piv = i;
        for (int r = i + 1; r < 4; r++) if (fabs(A[r][i]) > fabs(A[piv][i])) piv = r;
        if (fabs(A[piv][i]) < 1e-9) return;
        for (int j = 0; j < 5; j++) { double t = A[i][j]; A[i][j] = A[piv][j]; A[piv][j] = t; }
        for (int r = 0; r < 4; r++) {
            if (r == i) continue;
            double k = A[r][i] / A[i][i];
            for (int j = i; j < 5; j++) A[r][j] -= k * A[i][j];
        }
    }
    double a = A[0][4] / A[0][0];
    if (a < 0.2) return;                          /* barely faded: no reliable colour */
    for (int c = 0; c < 3; c++) {
        double v = A[c + 1][4] / A[c + 1][c + 1] / a;
        v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
        if (!FOG.have_col) FOG.col[c] = (float)v;
        else FOG.col[c] += ((float)v - FOG.col[c]) * 0.1f;
    }
    FOG.have_col = 1;
}

static void fog_sample(const gpu_poly_t *p, const hw_pv_t *pv) {
    if (!FOG.w || !p->textured || FOG.n_prev < 2 || FOG.k >= FOG.n_prev) return;
    s32 cx = (p->clut & 63) * 16, cy = (p->clut >> 6) & 511;
    if (cx < FOG.dx || cx >= FOG.dx + FOG.w || cy < FOG.dy || cy >= FOG.dy + FOG.h) return;
    for (int i = 0; i < (p->nverts == 4 ? 4 : 3); i++) {
        if (!pv[i].ok) continue;
        float z = pv[i].v.vz;
        if (z > FOG.hi) FOG.hi = z;
        if (FOG.k == FOG.n_prev - 1 && z < FOG.lo) FOG.lo = z;
    }
}

static void fog_frame_end(void) {
    if (FOG.hi > 0.0f && FOG.lo < 1e29f && FOG.hi > FOG.lo + 1000.0f) {
        if (!FOG.have) { FOG.fs = FOG.lo; FOG.fe = FOG.hi; FOG.have = 1; }
        else { FOG.fs += (FOG.lo - FOG.fs) * 0.1f; FOG.fe += (FOG.hi - FOG.fe) * 0.1f; }
    }
    if (FOG.k >= 2) fog_colour();
    FOG.n_prev = FOG.k;
    FOG.k = 0;
    FOG.lo = 1e30f; FOG.hi = 0.0f;
}

static int band_candidate(const gpu_sw_state_t *st, const PSX_RECT *src) {
    return !(src->x <= st->area_x1 && src->x + src->w > st->area_x0 &&
             src->y <= st->area_y1 && src->y + src->h > st->area_y0);
}

static int band_same(const band_t *b, const PSX_RECT *src, s32 dx, s32 dy) {
    return b->dx == dx && b->dy == dy && b->src.w == src->w && b->src.h == src->h;
}

static void band_note(const PSX_RECT *src, s32 dx, s32 dy) {
    for (int i = 0; i < nband_cur; i++)
        if (band_same(&band_cur[i], src, dx, dy)) {
            band_cur[i].src = *src;
            band_cur[i].n++;
            return;
        }
    if (nband_cur < MAX_BANDS) {
        band_cur[nband_cur].src = *src;
        band_cur[nband_cur].dx = dx;
        band_cur[nband_cur].dy = dy;
        band_cur[nband_cur].n = 1;
        nband_cur++;
    }
}

static const PSX_RECT *band_nearest(const PSX_RECT *src, s32 dx, s32 dy) {
    for (int i = 0; i < nband_near; i++)
        if (band_same(&band_near[i], src, dx, dy)) return &band_near[i].src;
    return NULL;
}

static int in_world(void) {
    return G.phase == PH_TERRAIN || G.phase == PH_OBJECTS;
}

#define ROOF_MIN 16
static u32 roof_sig;

static int roofed(void) {
    return hw_map.valid && roof_sig == hw_map.signature;
}

static u32 fx_flags_now(void) {
    u32 f = 0;
    if (ac3_hd.far_terrain && ac3_hd.atmosphere) f |= FX_FAR;
    if (ac3_hd.ocean)      f |= FX_OCEAN;
    if (ac3_hd.clouds)     f |= FX_CLOUDS;
    if (ac3_hd.forest)     f |= FX_FOREST;
    if (ac3_hd.atmosphere) f |= FX_ATMO;
    if (ac3_hd.gi)         f |= FX_GI;
    if (ac3_hd.sun)        f |= FX_SUN;
    if (roofed())          f &= ~(FX_CLOUDS | FX_SUN);
    return f;
}

enum { B_POLY, B_RECT, B_FILL, B_COPY };
typedef struct {
    u8 type, mode, skip;
    float cx, cy;
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
static struct { int sprites, rings, strip, hidden, roof, lists, sun, sea, deferred; } gstat;
static int glue_verbose;
static struct {
    int polys, all, none, mixed, semi, semi_precise, semi_ground, sealevel, lines, rects, rsemi;
    u32 keys[16]; int keyn[16];
} wd;
static float diag[4096];
static int ndiag;
static int diag_cmp(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static int buffering(void) {
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

static int poly_key_same(const gpu_poly_t *a, const gpu_poly_t *b) {
    return a->textured == b->textured && a->nverts == b->nverts && a->tpage == b->tpage &&
           a->clut == b->clut && a->semitrans == b->semitrans &&
           a->r[0] == b->r[0] && a->g[0] == b->g[0] && a->b[0] == b->b[0];
}

static int poly_has(const gpu_poly_t *q, s16 x, s16 y) {
    for (int k = 0; k < 4; k++) if (q->x[k] == x && q->y[k] == y) return 1;
    return 0;
}

static int sky_geometry(const bop_t *b) {
    return b->type == B_POLY && b->poly.textured && !b->poly.semitrans;
}

static int sky_roof(const bop_t *b) {
    if (!sky_geometry(b)) return 0;
    const float *R = G.scene.rot;
    for (int k = 0; k < (b->poly.nverts == 4 ? 4 : 3); k++) {
        const hw_pv_t *v = &b->pv[k];
        if (!v->ok || R[1] * v->v.vx + R[4] * v->v.vy + R[7] * v->v.vz >= 0.0f) return 0;
    }
    return 1;
}

static void flush_ops(int phase) {
    float ws = G.scene.valid ? G.scene.ws : 1.0f;
    if (phase == PH_SKY && G.scene.valid && hw_map.valid) {
        int roof = 0;
        for (int i = 0; i < nbops; i++) roof += sky_roof(&bops[i]);
        gstat.roof += roof;
        if (roof >= ROOF_MIN && !roofed()) {
            roof_sig = hw_map.signature;
            printf("enhanced: the sky has a roof over this map -- no clouds or sun under it\n");
        }
    }
    int far_on = G.fx && (fx_flags_now() & FX_FAR) && hw_map.valid;
    int sun_on = G.fx && (fx_flags_now() & FX_SUN);
    float sun_x = 0, sun_y = 0;
    int sun_ahead = 0;
    if (sun_on && G.scene.valid) {
        float v[3];
        for (int i = 0; i < 3; i++)
            v[i] = G.scene.rot[3 * i] * G.scene.sun[0] + G.scene.rot[3 * i + 1] * G.scene.sun[1] +
                   G.scene.rot[3 * i + 2] * G.scene.sun[2];
        if (v[2] > 0.02f) {
            sun_x = G.scene.H * G.scene.ws * v[0] / v[2];
            sun_y = G.scene.H * v[1] / v[2];
            sun_ahead = 1;
        }
    }
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
    if (sun_on)
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
    nbops = 0;
}

float hw_sun_test[2];
int hw_sun_test_on;

static void sun_test(hw_scene_t *s) {
    float d[3] = { hw_sun_test[0] * 160.0f / (s->H * s->ws), hw_sun_test[1] * 120.0f / s->H, 1.0f };
    float w[3], l = 0;
    for (int i = 0; i < 3; i++) {
        w[i] = s->rot[i] * d[0] + s->rot[3 + i] * d[1] + s->rot[6 + i] * d[2];
        l += w[i] * w[i];
    }
    l = sqrtf(l);
    for (int i = 0; i < 3; i++) s->sun[i] = w[i] / l;
}

float hw_cam_test[5];
int hw_cam_test_on;
int hw_cam_sweep_n = 1;
float hw_cam_sweep_d[5];

static void cam_test(hw_scene_t *s) {
    float yaw = hw_cam_test[3] * 0.0174533f, pitch = hw_cam_test[4] * 0.0174533f;
    float f[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
    float r[3] = { f[2], 0.0f, -f[0] };
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl; r[2] /= rl;
    float d[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    for (int i = 0; i < 3; i++) {
        s->rot[i] = r[i];
        s->rot[3 + i] = d[i];
        s->rot[6 + i] = f[i];
        s->cam[i] = hw_cam_test[i];
    }
}

enum { D_POLY, D_RECT, D_LINE, D_COPY };
typedef struct {
    u8 type;
    u32 order;
    gpu_sw_state_t st;
    gpu_poly_t poly;
    hw_pv_t pv[4];
    gpu_rect_t rect;
    PSX_RECT r;
    s32 dx, dy;
    int line;
} dop_t;
static dop_t *dops;
static int ndops, capdops;
static gpu_line_t *dlines;
static int ndlines, capdlines;

static int deferring(void) {
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

static void flush_deferred(void) {
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

static void begin_world(void) {
    G.begun = 1;
    if (!G.fx) return;
    if (hw_cam_test_on) {
        cam_test(&G.scene);
        if (hw_sun_test_on) {
            float az = hw_sun_test[0] * 0.0174533f, el = hw_sun_test[1] * 0.0174533f;
            G.scene.sun[0] = sinf(az) * cosf(el);
            G.scene.sun[1] = -sinf(el);
            G.scene.sun[2] = cosf(az) * cosf(el);
        }
    }
    G.scene.ofs[0] = (float)gpu_sw.ofs_x;
    G.scene.ofs[1] = (float)gpu_sw.ofs_y;
    G.scene.area[0] = gpu_sw.area_x0; G.scene.area[1] = gpu_sw.area_y0;
    G.scene.area[2] = gpu_sw.area_x1; G.scene.area[3] = gpu_sw.area_y1;
    G.scene.time = (float)G.fields / 60.0f;
    G.scene.frame = G.fields;
    if (hw_map.valid && hw_map.has_sea) G.scene.sea_y = hw_map.sea_y;
    hw_fx_flags = fx_flags_now();
    const band_t *pb = NULL;
    for (int i = 0; i < nband_near; i++)
        if (!pb || band_near[i].src.w * band_near[i].src.h > pb->src.w * pb->src.h)
            pb = &band_near[i];
    if (pb && ac3_hd.atmosphere)
        hw_fx_palette(pb->dx, pb->dy, pb->src.w, pb->src.h, pb->src.x, pb->src.y);
    else
        hw_fx_palette(0, 0, 0, 0, 0, 0);
    hw_fx_begin(&G.scene);
    if (ac3_hd.atmosphere && !ac3_hd.game_fog && nband_near) {
        int done[MAX_BANDS] = { 0 };
        for (int k = 0; k < nband_near; k++) {
            int best = -1;
            for (int i = 0; i < nband_near; i++)
                if (!done[i] && (best < 0 || band_near[i].src.w * band_near[i].src.h <
                                              band_near[best].src.w * band_near[best].src.h))
                    best = i;
            done[best] = 1;
            hw_copy(&gpu_sw, &band_near[best].src, band_near[best].dx, band_near[best].dy);
        }
    }
}

static void end_world(void) {
    G.ended = 1;
    if (!G.fx || !G.begun) { ndops = ndlines = 0; return; }
    hw_fx_end(&G.scene);
    flush_deferred();
}

/* Widescreen shows more of the world to the left and right than the game
 * thinks is on screen, so it leaves out the terrain tiles there. With world
 * effects on, the far-terrain pass paints the ground underneath and hides the
 * gaps. Without it (effects off, or not available yet in the browser build)
 * the gaps would show pixels left over from earlier frames as smeared streaks.
 * So the HD picture's drawing area is first cleared to the haze colour. Only
 * the HD picture: the game's own VRAM is not touched. */
static void widescreen_backdrop(void) {
    G.backdrop = 1;
    if (!hw_glue_widescreen_active()) return;
    if (G.fx && (fx_flags_now() & FX_FAR) && hw_map.valid && hw_fx_ready()) return;
    PSX_RECT r = { (s16)gpu_sw.area_x0, (s16)gpu_sw.area_y0,
                   (s16)(gpu_sw.area_x1 - gpu_sw.area_x0 + 1), (s16)(gpu_sw.area_y1 - gpu_sw.area_y0 + 1) };
    if (r.w <= 0 || r.h <= 0) return;
    u16 c[3];
    for (int i = 0; i < 3; i++) {
        float v = G.fog[i] < 0.0f ? 0.0f : G.fog[i] > 1.0f ? 1.0f : G.fog[i];
        c[i] = (u16)lrintf(v * 31.0f);
    }
    hw_fill(&r, (u16)(c[0] | (c[1] << 5) | (c[2] << 10)));
}

static void glue_phase(int ph) {
    if (!G.flight || ph == G.phase) return;
    if (!G.backdrop && ph >= PH_SKY && ph <= PH_OBJECTS) widescreen_backdrop();
    int old = G.phase;
    if (old == PH_SKY || old == PH_FLARE) flush_ops(old);
    if (old == PH_SKY) {
        memcpy(G.scene.fog_col, G.fog, sizeof G.fog);
        hw_world_sky_finish(&G.scene);
    }
    G.phase = ph;
    if (!G.begun && (ph == PH_TERRAIN || ph == PH_OBJECTS)) begin_world();
    if (!G.ended && (ph == PH_FLARE || ph == PH_HUD)) end_world();
}

static void glue_list_begin(const hw_scene_t *cam) {
    G.flight = cam && cam->valid;
    G.phase = PH_NONE;
    G.begun = G.ended = 0;
    G.backdrop = 0;
    G.band_copies = 0;
    G.order = 0;
    hw_draw_order(0);
    nbops = 0;
    ndops = ndlines = 0;
    nband_cur = 0;
    if (!G.flight) { G.fx = 0; return; }
    G.scene = *cam;
    if (hw_sun_test_on) sun_test(&G.scene);
    memcpy(G.scene.fog_col, G.fog, sizeof G.fog);
    FOG.w = FOG.h = 0;
    for (int i = 0; i < nband_near; i++)
        if (band_near[i].src.w * band_near[i].src.h > FOG.w * FOG.h) {
            FOG.dx = band_near[i].dx; FOG.dy = band_near[i].dy;
            FOG.w = band_near[i].src.w; FOG.h = band_near[i].src.h;
        }
    G.scene.fog_z[0] = G.scene.fog_z[1] = 0.0f;
    if (ac3_hd.game_fog && FOG.have && FOG.have_col) {
        G.scene.fog_z[0] = FOG.fs; G.scene.fog_z[1] = FOG.fe;
        memcpy(G.scene.game_fog_col, FOG.col, sizeof FOG.col);
    }
    hw_world_sky_begin(&G.scene);
    G.fx = ac3_hd.fx && hw_map.valid;
}

static void glue_list_end(void) {
    if (G.flight) {
        if (G.phase == PH_SKY || G.phase == PH_FLARE) flush_ops(G.phase);
        if (G.phase == PH_SKY) hw_world_sky_finish(&G.scene);
        if (G.begun && !G.ended) end_world();
        if (G.band_copies >= 4) {
            fog_frame_end();
            nband_near = 0;
            for (int i = 0; i < nband_cur; i++)
                if (band_cur[i].n >= 3) band_near[nband_near++] = band_cur[i];
        }
        if (glue_verbose)
            printf("  list: sky %u colour samples; %d sprite, %d ring and %d strip quads "
                   "(%d hidden), %d roof polygons%s; %d of the game's sun and flare and %d sea "
                   "polygons left out; %d effects drawn last; %d fog-band copies%s; effects %s\n",
                   G.scene.sky_samples, gstat.sprites, gstat.rings / 4 * 4, gstat.strip,
                   gstat.hidden, gstat.roof, roofed() ? " (roofed: no clouds or sun)" : "",
                   gstat.sun, gstat.sea, gstat.deferred, G.band_copies,
                   nband_near ? ", nearest band known" : "",
                   G.fx && G.begun ? (G.ended ? "drawn" : "begun") : "off");
        memset(&gstat, 0, sizeof gstat);
        if (glue_verbose && wd.polys) {
            printf("  world: %d polygons, %d all precise, %d none, %d mixed; %d semi-transparent "
                   "(%d with depth, %d on the ground); %d lines, %d rects (%d semi); %d at sea level:",
                   wd.polys, wd.all, wd.none, wd.mixed, wd.semi, wd.semi_precise, wd.semi_ground,
                   wd.lines, wd.rects, wd.rsemi, wd.sealevel);
            for (int k = 0; k < 16 && wd.keyn[k]; k++)
                printf(" %04X/%04X x%d", wd.keys[k] >> 16, wd.keys[k] & 0xFFFFu, wd.keyn[k]);
            printf(" (sea %04X/%04X)\n", hw_map.sea_tex >> 16, hw_map.sea_tex & 0xFFFFu);
        }
        memset(&wd, 0, sizeof wd);
        if (glue_verbose && ndiag > 20) {
            qsort(diag, (size_t)ndiag, sizeof diag[0], diag_cmp);
            printf("  camera check: map depth / GTE depth over %d terrain vertices: "
                   "10%% %.3f, median %.3f, 90%% %.3f\n", ndiag, diag[ndiag / 10],
                   diag[ndiag / 2], diag[ndiag * 9 / 10]);
        }
        ndiag = 0;
    }
    G.flight = 0;
    G.fx = 0;
    G.phase = PH_NONE;
}

static void terrain_depth(const gpu_poly_t *p, hw_pv_t *pv) {
    int n = p->nverts == 4 ? 4 : 3;
    float zmin = 1e30f, zmax = 0.0f;
    for (int i = 0; i < n; i++)
        if (pv[i].ok) {
            if (pv[i].v.vz < zmin) zmin = pv[i].v.vz;
            if (pv[i].v.vz > zmax) zmax = pv[i].v.vz;
        }
    if (zmax <= 0.0f) return;
    for (int i = 0; i < n; i++) {
        if (pv[i].ok) continue;
        float sx = (float)sx11(p->x[i]), sy = (float)sx11(p->y[i]);
        float z = hw_world_raycast(&G.scene, sx, sy, 32.0f, 60000.0f);
        if (z <= 0.0f || z < zmin * 0.5f || z > zmax * 2.0f) continue;
        pv[i].ok = 1;
        pv[i].v.sx = sx;
        pv[i].v.sy = sy;
        pv[i].v.vz = z;
        pv[i].v.vx = sx * z / (G.scene.H * G.scene.ws);
        pv[i].v.vy = sy * z / G.scene.H;
    }
}

static int game_sea(const gpu_poly_t *p, const hw_pv_t *pv) {
    if (!p->textured || !hw_fx_ocean_drawn() || !hw_world_sea_texture(p->tpage, p->clut)) return 0;
    const float *R = G.scene.rot;
    int n = p->nverts == 4 ? 4 : 3, ok = 0;
    for (int i = 0; i < n; i++) {
        if (!pv[i].ok) continue;
        float y = R[1] * pv[i].v.vx + R[4] * pv[i].v.vy + R[7] * pv[i].v.vz + G.scene.cam[1];
        if (fabsf(y - hw_map.sea_y) > 12.0f) return 0;
        ok++;
    }
    return ok > 0;
}

static void diag_world(const gpu_poly_t *p, const hw_pv_t *pv) {
    int n = p->nverts == 4 ? 4 : 3, ok = 0, sea = 0, ground = 0;
    for (int i = 0; i < n; i++) {
        if (!pv[i].ok) continue;
        ok++;
        const float *R = G.scene.rot;
        float w[3];
        for (int k = 0; k < 3; k++)
            w[k] = R[k] * pv[i].v.vx + R[3 + k] * pv[i].v.vy + R[6 + k] * pv[i].v.vz + G.scene.cam[k];
        if (hw_map.has_sea && fabsf(w[1] - hw_map.sea_y) < 12.0f) sea++;
        if (hw_map.valid && fabsf(w[1] - hw_world_ground(w[0], w[2], NULL)) < 30.0f) ground++;
    }
    wd.polys++;
    if (ok == n) wd.all++; else if (!ok) wd.none++; else wd.mixed++;
    if (p->semitrans) {
        wd.semi++;
        if (ok) wd.semi_precise++;
        if (ok && ground == ok) wd.semi_ground++;
    }
    if (ok && sea == ok && p->textured) {
        wd.sealevel++;
        u32 key = ((u32)(p->tpage & 0x19Fu) << 16) | p->clut;
        int k;
        for (k = 0; k < 16 && wd.keyn[k] && wd.keys[k] != key; k++) ;
        if (k < 16) { wd.keys[k] = key; wd.keyn[k]++; }
    }
}

static void glue_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t *pv_in) {
    hw_draw_order(G.flight && in_world() ? ++G.order : 0);
    hw_pv_t pv[4];
    if (pv_in) memcpy(pv, pv_in, sizeof pv);
    else memset(pv, 0, sizeof pv);
    if (G.flight) {
        if (G.phase == PH_SKY && !p->textured)
            for (int i = 0; i < (p->nverts == 4 ? 4 : 3); i++)
                hw_world_sky_add(&G.scene, (float)sx11(p->x[i]), (float)sx11(p->y[i]),
                                 p->r[i], p->g[i], p->b[i]);
        if (in_world() && G.fx && hw_map.valid) {
            terrain_depth(p, pv);
            fog_sample(p, pv);
            if (glue_verbose) diag_world(p, pv);
            if (G.begun && game_sea(p, pv)) { gstat.sea++; return; }
        }
        if (hw_cam_test_on && in_world()) return;
        if (p->semitrans && deferring()) {
            dop_t *d = dop_new(D_POLY);
            if (d) {
                d->st = *st;
                d->poly = *p;
                memcpy(d->pv, pv, sizeof pv);
                return;
            }
        }
        if (glue_verbose && hw_map.valid && G.phase >= PH_TERRAIN && G.phase <= PH_OBJECTS &&
            p->textured && ndiag < 4096)
            for (int i = 0; i < (p->nverts == 4 ? 4 : 3); i++) {
                if (!pv[i].ok || pv[i].v.vz < 800.0f || pv[i].v.vz > 25000.0f) continue;
                float zr = hw_world_raycast(&G.scene, pv[i].v.sx, pv[i].v.sy, 50.0f, 80000.0f);
                if (zr > 0.0f && ndiag < 4096) diag[ndiag++] = zr / pv[i].v.vz;
            }
    }
    if (buffering()) {
        bop_t *b = bop_new(B_POLY);
        if (b) {
            b->st = *st;
            b->poly = *p;
            memcpy(b->pv, pv, sizeof pv);
            return;
        }
    }
    hw_poly(st, p, pv);
}

static void glue_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    hw_draw_order(G.flight && in_world() ? ++G.order : 0);
    if (glue_verbose && G.flight && in_world()) { wd.rects++; if (r->semitrans) wd.rsemi++; }
    if (r->semitrans && deferring()) {
        dop_t *d = dop_new(D_RECT);
        if (d) { d->st = *st; d->rect = *r; return; }
    }
    if (G.flight && G.phase == PH_SKY && !r->textured && r->w >= 16 && r->h >= 16) {
        G.fog[0] = r->r / 255.0f;
        G.fog[1] = r->g / 255.0f;
        G.fog[2] = r->b / 255.0f;
    }
    if (buffering()) {
        bop_t *b = bop_new(B_RECT);
        if (b) { b->st = *st; b->rect = *r; return; }
    }
    hw_rect(st, r);
}

static void glue_line(const gpu_sw_state_t *st, const gpu_line_t *l) {
    hw_draw_order(G.flight && in_world() ? ++G.order : 0);
    if (glue_verbose && G.flight && in_world()) wd.lines++;
    if (l->semitrans && deferring()) {
        if (ndlines == capdlines) {
            int c = capdlines ? capdlines * 2 : 64;
            gpu_line_t *q = realloc(dlines, (size_t)c * sizeof *q);
            if (q) { dlines = q; capdlines = c; }
        }
        dop_t *d = ndlines < capdlines ? dop_new(D_LINE) : NULL;
        if (d) {
            d->st = *st;
            d->line = ndlines;
            dlines[ndlines++] = *l;
            return;
        }
    }
    if (buffering()) flush_ops(G.phase);
    hw_line(st, l);
}

static void glue_fill(const PSX_RECT *r, u16 pixel) {
    if (buffering()) {
        bop_t *b = bop_new(B_FILL);
        if (b) { b->r = *r; b->pixel = pixel; return; }
    }
    hw_fill(r, pixel);
}

static void glue_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy) {
    PSX_RECT s = *src;
    if (G.flight && in_world() && band_candidate(st, src)) {
        band_note(src, dx, dy);
        G.band_copies++;
        if (FOG.w && dx == FOG.dx && dy == FOG.dy && src->w == FOG.w && src->h == FOG.h) {
            if (!FOG.k) FOG.first = *src;
            FOG.last = *src;
            FOG.k++;
        }
        if (G.fx && ac3_hd.atmosphere && !ac3_hd.game_fog) {
            const PSX_RECT *n = band_nearest(src, dx, dy);
            if (n) s = *n;
        }
    }
    if (buffering()) {
        bop_t *b = bop_new(B_COPY);
        if (b) { b->st = *st; b->r = s; b->dx = dx; b->dy = dy; return; }
    }
    if (!getenv("AC3_NODEFERCOPY") && deferring() && s.x < st->area_x1 + 1 && s.x + s.w > st->area_x0 &&
        s.y < st->area_y1 + 1 && s.y + s.h > st->area_y0) {
        dop_t *d = dop_new(D_COPY);
        if (d) { d->st = *st; d->r = s; d->dx = dx; d->dy = dy; return; }
    }
    hw_copy(st, &s, dx, dy);
}

static void glue_upload(const PSX_RECT *r) {
    if (buffering()) flush_ops(G.phase);
    hw_upload(r, ac3_vram);
}

#define PV_CAND 32
static void resolve_pv(const gpu_poly_t *p, hw_pv_t pv[4], const hw_pv_t *fallback) {
    static ac3_pgxp_vtx_t c[4][PV_CAND];
    int n = p->nverts == 4 ? 4 : 3, nc[4], fixed[4];
    memset(pv, 0, sizeof(hw_pv_t) * 4);
    for (int i = 0; i < n; i++) {
        u32 key = ((u32)(u16)p->y[i] << 16) | (u16)p->x[i];
        u32 gen[PV_CAND];
        nc[i] = ac3_pgxp_lookup_all(key, c[i], gen, PV_CAND);
        if (!nc[i] && fallback && fallback[i].ok) { c[i][0] = fallback[i].v; nc[i] = 1; }
        fixed[i] = nc[i] == 1;
        if (fixed[i]) { pv[i].v = c[i][0]; pv[i].ok = 1; }
    }
    for (int round = 0; round < n; round++) {
        int pick = -1;
        for (int i = 0; i < n; i++)
            if (!fixed[i] && nc[i] > 1 && (pick < 0 || nc[i] < nc[pick])) pick = i;
        if (pick < 0) break;
        float cx = 0, cy = 0, cz = 0;
        int nf = 0;
        for (int i = 0; i < n; i++)
            if (fixed[i] && pv[i].ok) { cx += pv[i].v.vx; cy += pv[i].v.vy; cz += pv[i].v.vz; nf++; }
        float cost[PV_CAND];
        float best_d = 1e30f;
        for (int k = 0; k < nc[pick]; k++) {
            const ac3_pgxp_vtx_t *q = &c[pick][k];
            float d = 0;
            if (nf) {
                float dx = q->vx - cx / nf, dy = q->vy - cy / nf, dz = q->vz - cz / nf;
                d = dx * dx + dy * dy + dz * dz;
            } else {
                for (int j = 0; j < n; j++) {
                    if (j == pick || !nc[j]) continue;
                    float m = 1e30f;
                    for (int l = 0; l < nc[j]; l++) {
                        float dx = q->vx - c[j][l].vx, dy = q->vy - c[j][l].vy, dz = q->vz - c[j][l].vz;
                        float e = dx * dx + dy * dy + dz * dz;
                        if (e < m) m = e;
                    }
                    d += m;
                }
            }
            cost[k] = d;
            if (d < best_d) best_d = d;
        }
        int best = 0;
        for (int k = 0; k < nc[pick]; k++) {
            float tol = 0.05f * c[pick][k].vz;
            if (cost[k] <= best_d * 4.0f + tol * tol) { best = k; break; }
        }
        pv[pick].v = c[pick][best];
        pv[pick].ok = 1;
        fixed[pick] = 1;
    }
}

/* --glue-trace N: log every drawing operation of field N (development aid) */
u32 hw_glue_trace_field;
static int tracing(void) { return hw_glue_trace_field && G.fields >= hw_glue_trace_field && G.fields < hw_glue_trace_field + 3; }
static void trace_box(const char *what, const s16 *x, const s16 *y, int n, const char *extra) {
    if (!tracing()) return;
    int x0 = 99999, y0 = 99999, x1 = -99999, y1 = -99999;
    for (int i = 0; i < n; i++) {
        if (x[i] < x0) x0 = x[i]; if (x[i] > x1) x1 = x[i];
        if (y[i] < y0) y0 = y[i]; if (y[i] > y1) y1 = y[i];
    }
    printf("trace f%u ph%d area(%d,%d-%d,%d) ofs(%d,%d) %s box %d,%d-%d,%d %s\n", G.fields, G.phase,
           gpu_sw.area_x0, gpu_sw.area_y0, gpu_sw.area_x1, gpu_sw.area_y1, gpu_sw.ofs_x, gpu_sw.ofs_y,
           what, x0, y0, x1, y1, extra);
}

static void g_poly(const gpu_poly_t *p) {
    if (tracing()) {
        char e[96];
        snprintf(e, sizeof e, "%s%s rgb %02x%02x%02x tpage %04x clut %04x", p->textured ? "tex" : "flat", p->semitrans ? " semi" : "",
                 p->r[0], p->g[0], p->b[0], p->tpage, p->clut);
        trace_box("poly", p->x, p->y, p->nverts == 4 ? 4 : 3, e);
    }
    hw_pv_t pv[4];
    memset(pv, 0, sizeof pv);
    if (ac3_hd.pgxp) resolve_pv(p, pv, NULL);
    if (rec.active) {
        rec_head(R_POLY, sizeof gpu_sw + sizeof *p + sizeof pv);
        rec_put(&gpu_sw, sizeof gpu_sw);
        rec_put(p, sizeof *p);
        rec_put(pv, sizeof pv);
    }
    glue_poly(&gpu_sw, p, pv);
}

static void g_rect(const gpu_rect_t *r) {
    if (tracing()) {
        s16 x[2] = { (s16)r->x, (s16)(r->x + r->w - 1) }, y[2] = { (s16)r->y, (s16)(r->y + r->h - 1) };
        char e[96];
        snprintf(e, sizeof e, "%s%s rgb %02x%02x%02x tpage %04x clut %04x", r->textured ? "tex" : "flat", r->semitrans ? " semi" : "",
                 r->r, r->g, r->b, r->tpage, r->clut);
        trace_box("rect", x, y, 2, e);
    }
    if (rec.active) {
        rec_head(R_RECT, sizeof gpu_sw + sizeof *r);
        rec_put(&gpu_sw, sizeof gpu_sw);
        rec_put(r, sizeof *r);
    }
    glue_rect(&gpu_sw, r);
}

static void g_line(const gpu_line_t *l) {
    if (rec.active) {
        rec_head(R_LINE, sizeof gpu_sw + sizeof *l);
        rec_put(&gpu_sw, sizeof gpu_sw);
        rec_put(l, sizeof *l);
    }
    glue_line(&gpu_sw, l);
}

static void g_fill(const PSX_RECT *r, u16 pixel) {
    if (tracing()) {
        s16 x[2] = { r->x, (s16)(r->x + r->w - 1) }, y[2] = { r->y, (s16)(r->y + r->h - 1) };
        char e[32];
        snprintf(e, sizeof e, "pixel %04x", pixel);
        trace_box("FILL", x, y, 2, e);
    }
    if (rec.active) {
        rec_head(R_FILL, sizeof *r + 2);
        rec_put(r, sizeof *r);
        rec_put(&pixel, 2);
    }
    glue_fill(r, pixel);
}

static void g_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    if (tracing()) {
        s16 x[2] = { (s16)dx, (s16)(dx + src->w - 1) }, y[2] = { (s16)dy, (s16)(dy + src->h - 1) };
        char e[48];
        snprintf(e, sizeof e, "from %d,%d", src->x, src->y);
        trace_box("COPY", x, y, 2, e);
    }
    if (rec.active) {
        rec_head(R_COPY, sizeof gpu_sw + sizeof *src + 8);
        rec_put(&gpu_sw, sizeof gpu_sw);
        rec_put(src, sizeof *src);
        rec_put(&dx, 4);
        rec_put(&dy, 4);
    }
    glue_copy(&gpu_sw, src, dx, dy);
}

static void g_upload(const PSX_RECT *r) {
    if (tracing()) {
        s16 x[2] = { r->x, (s16)(r->x + r->w - 1) }, y[2] = { r->y, (s16)(r->y + r->h - 1) };
        trace_box("UPLOAD", x, y, 2, "");
    }
    if (rec.active && r->w > 0 && r->h > 0) {
        u32 n = (u32)r->w * (u32)r->h;
        rec_head(R_UPLOAD, sizeof *r + n * 2);
        rec_put(r, sizeof *r);
        for (s32 y = 0; y < r->h; y++)
            for (s32 x = 0; x < r->w; x++)
                rec_put(vram_at((r->x + x) & 1023, (r->y + y) & 511), 2);
    }
    glue_upload(r);
}

static void g_list(int begin) {
    hw_scene_t cam;
    memset(&cam, 0, sizeof cam);
    if (begin) {
        G.ot[0] = G.ot[1] = 0;
        if (in_flight()) {
            u32 world = MEM_R32(GAME_GP + 736);
            if ((world >> 24) == 0x80u) {
                G.ot[0] = MEM_R32(world + 24);
                G.ot[1] = MEM_R32(world + 28);
            }
            if (ac3_hd.fx) hw_world_map_update();
            hw_world_camera(&cam);
        }
    }
    if (rec.active) {
        u32 b = (u32)begin;
        rec_head(R_LIST, 4 + sizeof cam);
        rec_put(&b, 4);
        rec_put(&cam, sizeof cam);
    }
    if (begin) glue_list_begin(&cam);
    else glue_list_end();
}

static void g_node(u32 addr, int len) {
    if (!G.flight || len != 0) return;
    int ph = -1;
    for (int k = 0; k < 2; k++) {
        u32 base = G.ot[k];
        if (!base || addr < base || addr >= base + 4u * 4096u || ((addr - base) & 3u)) continue;
        u32 slot = (addr - base) >> 2;
        ph = slot == 4095 ? PH_SKY : slot == 4094 ? PH_TERRAIN : slot >= 2 ? PH_OBJECTS
           : slot == 1 ? PH_FLARE : PH_HUD;
    }
    if (ph < 0 || ph == G.phase) return;
    if (tracing()) printf("trace f%u phase %d -> %d\n", G.fields, G.phase, ph);
    if (rec.active) {
        u32 p = (u32)ph;
        rec_head(R_PHASE, 4 + sizeof gpu_sw);
        rec_put(&p, 4);
        rec_put(&gpu_sw, sizeof gpu_sw);
    }
    glue_phase(ph);
}

static const gpu_sw_hooks_t hooks = { g_poly, g_rect, g_line, g_fill, g_copy, g_upload,
                                      g_list, g_node };

void hw_glue_attach(void) {
    gpu_sw_hooks = &hooks;
    ac3_pgxp_enabled = ac3_hd.pgxp;
    ac3_ws_factor = 1.0f;
    hw_fx_flags = fx_flags_now();
}

int hw_glue_widescreen_active(void) {
    return ac3_ws_factor != 1.0f;
}

void hw_set_pgxp(int on) {
    ac3_hd.pgxp = on ? 1 : 0;
    ac3_pgxp_enabled = ac3_hd.pgxp;
}

void hw_glue_toggle_fx(void) {
    ac3_hd.fx = !ac3_hd.fx;
    printf("enhanced: world effects %s\n", ac3_hd.fx ? "on" : "off");
}

void hw_glue_frame_end(const gpu_display_t *d) {
    ac3_pgxp_next_frame();
    ac3_ws_factor = (ac3_hd.widescreen && in_flight()) ? 0.75f : 1.0f;
    ac3_gte_no_depthcue = ac3_hd.fx && ac3_hd.atmosphere && hw_map.valid && in_flight();
    if (hw_glue_trace_field && G.fields + 5 >= hw_glue_trace_field && G.fields < hw_glue_trace_field + 3)
        printf("trace: end of glue field %u (flight %d)\n", G.fields, G.flight);
    G.fields++;
    if (rec.active) {
        rec_head(R_FIELD, sizeof *d);
        rec_put(d, sizeof *d);
        if (--rec.fields_left <= 0) rec_finish();
    } else if (rec.armed) {
        rec_start();
    }
}

#ifndef AC3_HW_WEBGPU   /* headless renderer, HD replay and self-test: Vulkan-based debugging tools */
static struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qf;
    VkQueue q;
    VkCommandPool pool;
    VkCommandBuffer cb;
} HV;

static int headless_init(int scale) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ac3_enhanced";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, NULL, &HV.inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(HV.inst, &n, NULL);
    if (!n) return 0;
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(HV.inst, &n, devs);
    HV.phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && !HV.phys; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties qs[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs);
        for (uint32_t q = 0; q < qn; q++)
            if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { HV.phys = devs[i]; HV.qf = q; break; }
    }
    if (!HV.phys) return 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = HV.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feat = { 0 };
    feat.independentBlend = VK_TRUE;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &feat;
    if (vkCreateDevice(HV.phys, &di, NULL, &HV.dev) != VK_SUCCESS) return 0;
    vkGetDeviceQueue(HV.dev, HV.qf, 0, &HV.q);
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = HV.qf;
    if (vkCreateCommandPool(HV.dev, &pi, NULL, &HV.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = HV.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(HV.dev, &ai, &HV.cb);
    hw_config_t cfg = { scale, ac3_hd.perspective };
    return hw_init(HV.phys, HV.dev, HV.qf, &cfg);
}

static double flush_ms;

static void headless_flush(void) {
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&f0);
    vkResetCommandBuffer(HV.cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(HV.cb, &bi);
    hw_record(HV.cb, 0);
    vkEndCommandBuffer(HV.cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &HV.cb;
    vkQueueSubmit(HV.q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(HV.q);
    QueryPerformanceCounter(&f1);
    flush_ms = (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)fq.QuadPart;
}

static void headless_shutdown(void) {
    hw_shutdown();
    if (HV.pool) vkDestroyCommandPool(HV.dev, HV.pool, NULL);
    if (HV.dev) vkDestroyDevice(HV.dev, NULL);
    if (HV.inst) vkDestroyInstance(HV.inst, NULL);
    memset(&HV, 0, sizeof HV);
}

static void write_rgba_png(const char *path, const u8 *rgba, unsigned w, unsigned h) {
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    ac3_write_png_rgb(path, w, h, rgb);
    free(rgb);
}

static void write_sw_png(const char *path, int x, int y, int w, int h) {
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u16 p = *vram_at((x + i) & 1023, (y + j) & 511);
            u8 *o = rgb + ((size_t)j * w + i) * 3;
            o[0] = (u8)(((p & 31) << 3) | ((p & 31) >> 2));
            o[1] = (u8)((((p >> 5) & 31) << 3) | (((p >> 5) & 31) >> 2));
            o[2] = (u8)((((p >> 10) & 31) << 3) | (((p >> 10) & 31) >> 2));
        }
    ac3_write_png_rgb(path, (unsigned)w, (unsigned)h, rgb);
    free(rgb);
}

static void write_depth_png(const char *path, const float *z, unsigned w, unsigned h) {
    float zmax = 1.0f;
    for (size_t i = 0; i < (size_t)w * h; i++) if (z[i] > zmax && z[i] < 1.0e8f) zmax = z[i];
    u8 *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        u8 *o = rgb + i * 3;
        if (z[i] == 0.0f) { o[0] = 0; o[1] = 0; o[2] = 96; continue; }
        if (z[i] >= 1.0e8f) { o[0] = 64; o[1] = 0; o[2] = 0; continue; }
        float zz = z[i] < 0.0f ? -z[i] : z[i];
        float t = 1.0f - (float)(log((double)zz + 1.0) / log((double)zmax + 1.0));
        if (t < 0.0f) t = 0.0f;
        u8 v = (u8)(t * 255.0f);
        o[0] = z[i] < 0.0f ? 0 : v;
        o[1] = o[2] = v;
    }
    ac3_write_png_rgb(path, w, h, rgb);
    free(rgb);
}

extern void ac3_vram_init(void);

typedef struct { u8 t; u32 len; const u8 *p; } rec_ref_t;

enum { EV_BEGIN = 1, EV_TERRAIN = 2, EV_HUD = 4, EV_END = 8 };

static void guess_phases(const rec_ref_t *r, int n, u8 *ev) {
    size_t st = sizeof(gpu_sw_state_t), pl = sizeof(gpu_poly_t);
    for (int i = 0; i < n; ) {
        if (r[i].t == R_FIELD || r[i].t == R_GTE) { i++; continue; }
        int j = i, polys = 0, first_prec = -1, last_prec = -1, first_band = -1, first_line = -1;
        for (; j < n && r[j].t != R_FIELD; j++) {
            if (r[j].t == R_POLY) {
                polys++;
                hw_pv_t pv[4];
                memcpy(pv, r[j].p + st + pl, sizeof pv);
                if (pv[0].ok || pv[1].ok || pv[2].ok) {
                    if (first_prec < 0) first_prec = j;
                    last_prec = j;
                }
            } else if (r[j].t == R_LINE && first_band >= 0 && first_line < 0) {
                first_line = j;
            } else if (r[j].t == R_COPY && first_band < 0) {
                PSX_RECT s;
                memcpy(&s, r[j].p + st, sizeof s);
                if (s.w == 16 && (s.h == 64 || s.h == 16)) first_band = j;
            }
        }
        if (polys >= 64 && first_prec >= 0) {
            int terr = first_band >= 0 ? first_band : first_prec;
            ev[i] |= EV_BEGIN;
            ev[terr] |= EV_TERRAIN;
            int hud = last_prec + 1;
            if (first_line > terr && first_line < hud) hud = first_line;
            if (hud < j) ev[hud] |= EV_HUD;
            ev[j < n ? j : n - 1] |= EV_END;
        }
        i = j;
    }
}

static void prime_bands(const rec_ref_t *r, int n, const u8 *ev) {
    int ph = PH_NONE, copies = 0;
    size_t st = sizeof(gpu_sw_state_t);
    nband_cur = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i] & EV_TERRAIN) ph = PH_TERRAIN;
        if (ev[i] & EV_HUD) ph = PH_HUD;
        if (r[i].t == R_PHASE) { u32 p; memcpy(&p, r[i].p, 4); ph = (int)p; }
        if (r[i].t == R_LIST) { u32 b; memcpy(&b, r[i].p, 4); if (!b) ph = PH_NONE; }
        if (r[i].t == R_COPY && (ph == PH_TERRAIN || ph == PH_OBJECTS)) {
            PSX_RECT s; s32 dx, dy;
            gpu_sw_state_t gs;
            memcpy(&gs, r[i].p, st);
            memcpy(&s, r[i].p + st, sizeof s);
            memcpy(&dx, r[i].p + st + sizeof s, 4);
            memcpy(&dy, r[i].p + st + sizeof s + 4, 4);
            if (!band_candidate(&gs, &s)) continue;
            band_note(&s, dx, dy);
            copies++;
        }
    }
    nband_near = 0;
    if (copies >= 4)
        for (int i = 0; i < nband_cur; i++)
            if (band_cur[i].n >= 3) band_near[nband_near++] = band_cur[i];
    nband_cur = 0;
}

int hw_replay_main(const char *path, const char *out_prefix, const char *ram_path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 1; }
    fclose(f);
    const u32 *hdr = (const u32 *)data;
    if (size < 16 || hdr[0] != REC_MAGIC || hdr[1] < 1 || hdr[1] > REC_VERSION) {
        fprintf(stderr, "replay: %s is not a recording\n", path);
        return 1;
    }
    size_t vbytes = (size_t)GPU_VRAM_W * GPU_VRAM_H * 2;
    if ((size_t)size < 16 + vbytes) return 1;

    ac3_vram_init();
    memcpy(ac3_vram, data + 16, vbytes);
    gpu_sw_reset();
    if (!headless_init(ac3_hd.scale)) {
        fprintf(stderr, "replay: no Vulkan device\n");
        return 1;
    }
    int S = hw_scale();
    PSX_RECT all = { 0, 0, GPU_VRAM_W, GPU_VRAM_H };
    hw_upload(&all, ac3_vram);
    hw_fx_flags = fx_flags_now();
    printf("replay: %s, %d fields recorded, widescreen %s, scale %d\n",
           path, (int)hdr[2], hdr[3] ? "on" : "off", S);

    int have_ram = 0;
    if (ram_path) {
        FILE *rf = fopen(ram_path, "rb");
        if (rf) {
            have_ram = fread(ac3_ram, 1, AC3_RAM_SIZE, rf) == AC3_RAM_SIZE;
            fclose(rf);
        }
        printf("replay: game RAM from %s%s\n", ram_path, have_ram ? "" : " (unreadable)");
    }

    int nrec = 0, caprec = 4096;
    rec_ref_t *recs = malloc(sizeof *recs * (size_t)caprec);
    int has_lists = 0;
    for (size_t q = 16 + vbytes; q + 5 <= (size_t)size; ) {
        u8 t = data[q];
        u32 l;
        memcpy(&l, data + q + 1, 4);
        if (q + 5 + l > (size_t)size) break;
        if (nrec == caprec) { caprec *= 2; recs = realloc(recs, sizeof *recs * (size_t)caprec); }
        recs[nrec].t = t;
        recs[nrec].len = l;
        recs[nrec].p = data + q + 5;
        nrec++;
        if (t == R_LIST) has_lists = 1;
        if (t == R_RAM && l == AC3_RAM_SIZE) { memcpy(ac3_ram, data + q + 5, AC3_RAM_SIZE); have_ram = 1; }
        q += 5 + l;
    }
    u8 *ev = calloc((size_t)nrec + 1, 1);
    if (!has_lists && have_ram) guess_phases(recs, nrec, ev);
    prime_bands(recs, nrec, ev);

    static u32 gte_keys[1 << 17];
    static u8 gte_used[1 << 17];
    memset(gte_used, 0, sizeof gte_used);
    u32 n_gte = 0, v_total = 0, v_from_gte = 0;
    for (int i = 0; i < nrec; i++) {
        if (recs[i].t != R_GTE || recs[i].len < 4) continue;
        u32 k;
        memcpy(&k, recs[i].p, 4);
        u32 h = (k * 2654435761u) >> 15;
        while (gte_used[h] && gte_keys[h] != k) h = (h + 1) & ((1u << 17) - 1);
        if (!gte_used[h]) { gte_used[h] = 1; gte_keys[h] = k; n_gte++; }
    }

    int field = 0, map_png = 0, pass = 0;
    glue_verbose = 1;
    hw_world_verbose = 1;
next_pass:
    for (int i = 0; i < nrec; i++) {
        u8 type = recs[i].t;
        const u8 *p = recs[i].p;
        if (ev[i] & EV_BEGIN) {
            hw_scene_t cam;
            memset(&cam, 0, sizeof cam);
            ac3_ws_factor = hdr[3] ? 0.75f : 1.0f;
            if (ac3_hd.fx) hw_world_map_update();
            hw_world_camera(&cam);
            glue_list_begin(&cam);
            glue_phase(PH_SKY);
        }
        if (ev[i] & (EV_TERRAIN | EV_HUD)) {
            if (type == R_POLY || type == R_RECT || type == R_LINE || type == R_COPY)
                memcpy(&gpu_sw, p, sizeof gpu_sw);
            if (ev[i] & EV_TERRAIN) glue_phase(PH_TERRAIN);
            if (ev[i] & EV_HUD) glue_phase(PH_HUD);
        }
        if ((ev[i] & EV_END) && type == R_FIELD) glue_list_end();
        switch (type) {
        case R_POLY: {
            gpu_sw_state_t st; gpu_poly_t pr; hw_pv_t pv[4];
            memcpy(&st, p, sizeof st); memcpy(&pr, p + sizeof st, sizeof pr);
            memcpy(pv, p + sizeof st + sizeof pr, sizeof pv);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS"))) {
                static s32 la[4] = { -1, -1, -1, -1 };
                if (la[0] != st.area_x0 || la[1] != st.area_y0 || la[2] != st.area_x1 || la[3] != st.area_y1) {
                    printf("TMP area %d,%d-%d,%d ofs %d,%d (op %d)\n", st.area_x0, st.area_y0, st.area_x1, st.area_y1, st.ofs_x, st.ofs_y, i);
                    la[0] = st.area_x0; la[1] = st.area_y0; la[2] = st.area_x1; la[3] = st.area_y1;
                }
                int ymin = 9999, ymax = -9999;
                for (int k = 0; k < pr.nverts; k++) { int y = pr.y[k] + st.ofs_y; if (y < ymin) ymin = y; if (y > ymax) ymax = y; }
                int by = st.area_y0;
                if (G.phase == PH_FLARE)
                    printf("TMP flarepoly op %d n%d tex%d semi%d gour%d y %d..%d rgb %02x%02x%02x tpage %04x\n", i, pr.nverts, pr.textured, pr.semitrans, pr.gouraud, ymin, ymax, pr.r[0], pr.g[0], pr.b[0], pr.tpage);
                else if (ymin < by + 32 || ymax >= by + 192)
                    printf("TMP poly op %d n%d tex%d semi%d y %d..%d rgb %02x%02x%02x pv %d\n", i, pr.nverts, pr.textured, pr.semitrans, ymin - by, ymax - by, pr.r[0], pr.g[0], pr.b[0], pv[0].ok);
            }
            gpu_sw = st; gpu_sw_poly(&pr);
            if (n_gte && ac3_hd.pgxp) {
                hw_pv_t rec_pv[4];
                memcpy(rec_pv, pv, sizeof pv);
                resolve_pv(&pr, pv, rec_pv);
            }
            if (!ac3_hd.pgxp) memset(pv, 0, sizeof pv);
            glue_poly(&st, &pr, pv);
            if (n_gte)
                for (int k = 0; k < (pr.nverts == 4 ? 4 : 3); k++) {
                    u32 key = ((u32)(u16)pr.y[k] << 16) | (u16)pr.x[k];
                    u32 h = (key * 2654435761u) >> 15;
                    while (gte_used[h] && gte_keys[h] != key) h = (h + 1) & ((1u << 17) - 1);
                    v_total++;
                    if (gte_used[h]) v_from_gte++;
                }
            break;
        }
        case R_RECT: {
            gpu_sw_state_t st; gpu_rect_t r;
            memcpy(&st, p, sizeof st); memcpy(&r, p + sizeof st, sizeof r);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP rect op %d at %d,%d %ux%u tex%d semi%d rgb %02x%02x%02x area %d,%d-%d,%d\n", i, r.x + st.ofs_x, r.y + st.ofs_y, r.w, r.h, r.textured, r.semitrans, r.r, r.g, r.b, st.area_x0, st.area_y0, st.area_x1, st.area_y1);
            gpu_sw = st; gpu_sw_rect(&r);
            glue_rect(&st, &r);
            break;
        }
        case R_LINE: {
            gpu_sw_state_t st; static gpu_line_t l;
            memcpy(&st, p, sizeof st); memcpy(&l, p + sizeof st, sizeof l);
            gpu_sw = st; gpu_sw_line(&l);
            glue_line(&st, &l);
            break;
        }
        case R_FILL: {
            PSX_RECT r; u16 px;
            memcpy(&r, p, sizeof r); memcpy(&px, p + sizeof r, 2);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP fill op %d at %d,%d %dx%d px %04x\n", i, r.x, r.y, r.w, r.h, px);
            gpu_sw_fill(&r, px);
            glue_fill(&r, px);
            break;
        }
        case R_COPY: {
            gpu_sw_state_t st; PSX_RECT r; s32 dx, dy;
            memcpy(&st, p, sizeof st); memcpy(&r, p + sizeof st, sizeof r);
            memcpy(&dx, p + sizeof st + sizeof r, 4); memcpy(&dy, p + sizeof st + sizeof r + 4, 4);
            gpu_sw = st; gpu_sw_copy(&r, dx, dy);
            glue_copy(&st, &r, dx, dy);
            break;
        }
        case R_UPLOAD: {
            PSX_RECT r;
            memcpy(&r, p, sizeof r);
            const u8 *px = p + sizeof r;
            for (s32 y = 0; y < r.h; y++)
                for (s32 x = 0; x < r.w; x++)
                    memcpy(vram_at((r.x + x) & 1023, (r.y + y) & 511),
                           px + ((size_t)y * r.w + x) * 2, 2);
            glue_upload(&r);
            break;
        }
        case R_LIST: {
            u32 begin;
            hw_scene_t scene;
            memcpy(&begin, p, 4);
            memcpy(&scene, p + 4, sizeof scene);
            if (begin) {
                if (ac3_hd.fx && have_ram) hw_world_map_update();
                glue_list_begin(&scene);
            } else {
                glue_list_end();
            }
            break;
        }
        case R_PHASE: {
            u32 ph;
            memcpy(&ph, p, 4);
            memcpy(&gpu_sw, p + 4, sizeof gpu_sw);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP phase %u at op %d (area %d,%d-%d,%d) begun %d ended %d\n", ph, i, gpu_sw.area_x0, gpu_sw.area_y0, gpu_sw.area_x1, gpu_sw.area_y1, G.begun, G.ended);
            glue_phase((int)ph);
            break;
        }
        case R_GTE: {
            u32 key;
            ac3_pgxp_vtx_t v;
            if (recs[i].len < 4 + sizeof v) break;
            memcpy(&key, p, 4);
            memcpy(&v, p + 4, sizeof v);
            ac3_pgxp_record(key, &v);
            break;
        }
        case R_FIELD: {
            gpu_display_t d;
            memcpy(&d, p, sizeof d);
            ac3_pgxp_next_frame();
            headless_flush();
            G.fields++;
            u32 prims = 0, precise = 0;
            hw_frame_stats(&prims, &precise);
            char out[300];
            if (hw_map.valid && !map_png) {
                snprintf(out, sizeof out, "%s_map.png", out_prefix);
                hw_world_map_png(out);
                map_png = 1;
            }
            if (d.enabled && d.w > 0 && d.h > 0 && !d.rgb24) {
                unsigned W = (unsigned)(d.w * S), Hh = (unsigned)(d.h * S);
                u8 *rgba = malloc((size_t)W * Hh * 4);
                float *z = malloc((size_t)W * Hh * 4);
                if (rgba && z &&
                    hw_readback(HV.q, HV.pool, d.x, d.y, d.w, d.h, rgba) &&
                    hw_readback_depth(HV.q, HV.pool, d.x, d.y, d.w, d.h, z)) {
                    snprintf(out, sizeof out, "%s_f%d.png", out_prefix, field);
                    write_rgba_png(out, rgba, W, Hh);
                    if (hw_cam_sweep_n <= 1) {
                        snprintf(out, sizeof out, "%s_f%d_depth.png", out_prefix, field);
                        write_depth_png(out, z, W, Hh);
                    }
                }
                free(rgba); free(z);
                if (hw_cam_sweep_n <= 1) {
                    snprintf(out, sizeof out, "%s_f%d_native.png", out_prefix, field);
                    write_sw_png(out, d.x, d.y, d.w, d.h);
                }
                if (getenv("AC3_CLUTDUMP")) {
                    static u8 cb[64 * 4 * 16 * 4];
                    int SS = hw_scale();
                    if (hw_readback(HV.q, HV.pool, 352, 299, 16, 1, cb)) {
                        printf("HD clut row 299:");
                        for (int i = 0; i < 16; i++) { u8 *q = cb + (size_t)(i * SS) * 4; printf(" %02X%02X%02X/%d", q[0], q[1], q[2], q[3] > 127); }
                        printf("\nSW clut row 299:");
                        for (int i = 0; i < 16; i++) { u16 p = *vram_at(352 + i, 299); printf(" %02X%02X%02X/%d", (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3, p >> 15); }
                        printf("\n");
                    }
                    if (hw_readback(HV.q, HV.pool, 448, 256, 4, 16, cb)) {
                        printf("HD idx page:");
                        for (int j = 2; j < 8; j++) for (int i = 0; i < 4; i++) { u8 *q = cb + ((size_t)(j * SS) * (4 * SS) + (size_t)(i * SS)) * 4; printf(" %02X%02X", q[0], q[1]); }
                        printf("\nSW idx page:");
                        for (int j = 2; j < 8; j++) for (int i = 0; i < 4; i++) { u16 p = *vram_at(448 + i, 256 + j); printf(" %04X", p); }
                        printf("\n");
                    }
                }
            }
            printf("  field %d: display %d,%d %dx%d%s, %u primitives, %u with precise vertices,"
                   " %.1f ms on the GPU\n",
                   field, d.x, d.y, d.w, d.h, d.rgb24 ? " 24-bit" : "", prims, precise, flush_ms);
            field++;
            break;
        }
        default:
            break;
        }
    }
    if (++pass < hw_cam_sweep_n) {
        for (int k = 0; k < 5; k++) hw_cam_test[k] += hw_cam_sweep_d[k];
        memcpy(ac3_vram, data + 16, vbytes);
        hw_upload(&all, ac3_vram);
        glue_verbose = hw_world_verbose = 0;
        goto next_pass;
    }
    if (n_gte)
        printf("  the GTE made %u distinct projections; %u of %u polygon vertices are one of them\n",
               n_gte, v_from_gte, v_total);
    else
        printf("  (recorded before projections were captured)\n");
    headless_shutdown();
    free(ev);
    free(recs);
    free(data);
    return 0;
}

static int st_fail;

static void st_upload(int x, int y, int w, int h, const u16 *px) {
    PSX_RECT r = { (s16)x, (s16)y, (s16)w, (s16)h };
    gpu_sw_load_begin(&r);
    for (int i = 0; i < w * h; i += 2) {
        u32 lo = px[i], hi = (i + 1 < w * h) ? px[i + 1] : 0;
        gpu_sw_load_word(lo | (hi << 16));
    }
}

static void st_env(int x0, int y0, int x1, int y1, int ox, int oy, u16 texpage) {
    gpu_sw_gp0_state(0xE1000000u | texpage);
    gpu_sw_gp0_state(0xE3000000u | (u32)x0 | ((u32)y0 << 10));
    gpu_sw_gp0_state(0xE4000000u | (u32)x1 | ((u32)y1 << 10));
    gpu_sw_gp0_state(0xE5000000u | ((u32)ox & 0x7FF) | (((u32)oy & 0x7FF) << 11));
}

static u32 st_rng = 1234567u;
static u32 st_rand(void) { st_rng = st_rng * 1103515245u + 12345u; return st_rng >> 8; }

static u16 page4, page8, page15, page15n, clut4, clut8;

static void st_textures(void) {
    static u16 t4[64 * 256], t8[128 * 256], t15[256 * 256], n15[256 * 256];
    static u16 c4[16], c8[256];
    for (int v = 0; v < 256; v++)
        for (int w = 0; w < 64; w++) {
            u16 word = 0;
            for (int k = 0; k < 4; k++) {
                int u = w * 4 + k;
                word |= (u16)((((u >> 4) + (v >> 5)) & 15) << (k * 4));
            }
            t4[v * 64 + w] = word;
        }
    for (int i = 0; i < 16; i++)
        c4[i] = (u16)((i * 2) | ((31 - i) << 5) | ((8 + i) << 10) | ((i & 1) << 15));
    c4[0] = 0x0421;
    for (int v = 0; v < 256; v++)
        for (int w = 0; w < 128; w++) {
            int u0 = w * 2, u1 = w * 2 + 1;
            t8[v * 128 + w] = (u16)(((u0 + v) & 255) | (((u1 + v) & 255) << 8));
        }
    for (int i = 0; i < 256; i++)
        c8[i] = (u16)((i >> 3) | (((255 - i) >> 3) << 5) | (16 << 10) | ((i & 32) << 10));
    c8[0] = 0x0842;
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++) {
            t15[v * 256 + u] = (u16)((u >> 3) | ((v >> 3) << 5) | (((u + v) >> 4) << 10) |
                                     (((u >> 5) & 1) << 15));
            n15[v * 256 + u] = (u16)(st_rand() | 1u);
        }
    st_upload(640, 0, 64, 256, t4);
    st_upload(704, 0, 128, 256, t8);
    st_upload(768, 256, 256, 256, t15);
    st_upload(512, 256, 256, 256, n15);
    st_upload(0, 480, 16, 1, c4);
    st_upload(0, 481, 256, 1, c8);
    clut4 = (u16)(480 << 6);
    clut8 = (u16)(481 << 6);
    page4 = (u16)(10 | (0 << 7));
    page8 = (u16)(11 | (1 << 7));
    page15 = (u16)(12 | (1 << 4) | (2 << 7));
    page15n = (u16)(8 | (1 << 4) | (2 << 7));
}

static void st_clear(void) {
    PSX_RECT bg = { 0, 0, 320, 240 };
    gpu_sw_fill(&bg, 0x2108);
    st_env(0, 0, 319, 239, 0, 0, 0);
    gpu_sw_gp0_state(0xE6000000u);
}

static void st_poly(int nverts, int gouraud, int kind, int raw, int semi, int blend,
                    s16 cx, s16 cy, int rx, int ry) {
    gpu_poly_t p;
    memset(&p, 0, sizeof p);
    p.nverts = nverts;
    p.gouraud = gouraud;
    p.textured = kind != 0;
    p.raw = raw;
    p.semitrans = semi;
    u16 b = (u16)(blend << 5);
    p.tpage = (u16)((kind == 1 ? page4 : kind == 2 ? page8 : page15) | b);
    if (!p.textured) p.tpage = b;
    p.clut = kind == 1 ? clut4 : clut8;
    for (int i = 0; i < nverts; i++) {
        p.x[i] = (s16)(cx + (s16)(st_rand() % (2 * rx + 1)) - rx);
        p.y[i] = (s16)(cy + (s16)(st_rand() % (2 * ry + 1)) - ry);
        p.u[i] = (u8)st_rand(); p.v[i] = (u8)st_rand();
        p.r[i] = (u8)st_rand(); p.g[i] = (u8)st_rand(); p.b[i] = (u8)st_rand();
        if (!gouraud && i) { p.r[i] = p.r[0]; p.g[i] = p.g[0]; p.b[i] = p.b[0]; }
    }
    if (p.textured) gpu_sw_texpage_attr(p.tpage);
    else gpu_sw.texpage = (u16)((gpu_sw.texpage & ~0x60u) | b);
    gpu_sw_texwin(&p.twin_mask, &p.twin_off);
    gpu_sw_poly(&p);
}

static void scene_flat(void) {
    for (int n = 0; n < 120; n++)
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), 0, 0, 0, 0,
                (s16)(20 + st_rand() % 280), (s16)(20 + st_rand() % 200), 50, 40);
}

static void scene_textured(void) {
    for (int n = 0; n < 120; n++)
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), 1 + (int)(st_rand() % 3),
                (st_rand() & 3) == 0, 0, 0,
                (s16)(20 + st_rand() % 280), (s16)(20 + st_rand() % 200), 50, 40);
}

static void scene_blend(void) {
    for (int i = 0; i < 16; i++) {
        PSX_RECT r = { (s16)(i * 20), 0, 20, 240 };
        gpu_sw_fill(&r, (u16)((i * 2) | ((31 - i * 2) << 5) | ((i & 1) ? 0x7C00 : 0)));
    }
    for (int n = 0; n < 80; n++) {
        int kind = (int)(st_rand() % 4);
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), kind, 0, 1,
                (int)(st_rand() & 3), (s16)(20 + st_rand() % 280),
                (s16)(20 + st_rand() % 200), 45, 35);
    }
}

static void scene_sprites(void) {
    for (int n = 0; n < 60; n++) {
        gpu_rect_t r;
        memset(&r, 0, sizeof r);
        r.textured = 1;
        r.raw = (int)(st_rand() & 1);
        r.x = (s16)(st_rand() % 290); r.y = (s16)(st_rand() % 210);
        r.w = (u16)(1 + st_rand() % 60); r.h = (u16)(1 + st_rand() % 60);
        r.u = (u8)st_rand(); r.v = (u8)st_rand();
        r.r = (u8)(64 + st_rand() % 128); r.g = (u8)(64 + st_rand() % 128);
        r.b = (u8)(64 + st_rand() % 128);
        gpu_sw.texpage = (u16)((page15n & 0x1FF) |
                               ((n % 5 == 0) ? 0x1000 : 0) | ((n % 7 == 0) ? 0x2000 : 0));
        r.tpage = gpu_sw.texpage;
        gpu_sw_texwin(&r.twin_mask, &r.twin_off);
        gpu_sw_rect(&r);
    }
}

static void scene_mask(void) {
    gpu_sw_gp0_state(0xE6000001u);
    gpu_poly_t m;
    memset(&m, 0, sizeof m);
    m.nverts = 4;
    m.x[0] = 100; m.y[0] = 60; m.x[1] = 220; m.y[1] = 60;
    m.x[2] = 100; m.y[2] = 180; m.x[3] = 220; m.y[3] = 180;
    for (int i = 0; i < 4; i++) { m.r[i] = 255; m.g[i] = 64; m.b[i] = 0; }
    gpu_sw_poly(&m);
    gpu_sw_gp0_state(0xE6000002u);
    for (int i = 0; i < 4; i++) { m.x[i] = (s16)(m.x[i] - 50); m.y[i] = (s16)(m.y[i] - 40); m.g[i] = 255; }
    gpu_sw_poly(&m);
    for (int k = 0; k < 8; k++)
        st_poly(4, 1, (k & 1) ? 3 : 0, 0, 1, k >> 1,
                (s16)(90 + k * 20), (s16)(110 + (k & 3) * 12), 40, 30);
    gpu_sw_gp0_state(0xE6000000u);
}

static void scene_copy(void) {
    scene_flat();
    PSX_RECT src = { 40, 40, 64, 32 };
    gpu_sw_copy(&src, 200, 150);
    PSX_RECT src2 = { 0, 0, 16, 1 };
    gpu_sw_gp0_state(0xE6000001u);
    gpu_sw_copy(&src2, 32, 480);
    gpu_sw_gp0_state(0xE6000000u);
}

typedef struct {
    const char *name;
    void (*draw)(void);
    double min_ok;
    double min_exact;
} st_case_t;

static int st_run(const st_case_t *c, int S) {
    st_clear();
    c->draw();
    headless_flush();
    int W = 320, Hh = 240;
    u8 *px = malloc((size_t)W * S * Hh * S * 4);
    if (!px || !hw_readback(HV.q, HV.pool, 0, 0, W, Hh, px)) { free(px); return 0; }
    long in_total = 0, in_exact = 0, in_ok = 0, in_mask = 0, all_ok = 0;
    for (int y = 0; y < Hh; y++)
        for (int x = 0; x < W; x++) {
            size_t hy = (size_t)y * S + (size_t)(S / 2), hx = (size_t)x * S + (size_t)(S / 2);
            const u8 *h = px + (hy * (size_t)(W * S) + hx) * 4;
            u16 s = *vram_at(x, y);
            int dr = abs((h[0] >> 3) - (s & 31));
            int dg = abs((h[1] >> 3) - ((s >> 5) & 31));
            int db = abs((h[2] >> 3) - ((s >> 10) & 31));
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d <= 2) all_ok++;
            int interior = 1;
            for (int j = -1; j <= 1 && interior; j++)
                for (int i = -1; i <= 1; i++) {
                    int nx = x + i, ny = y + j;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= Hh) continue;
                    u16 n = *vram_at(nx, ny);
                    if (((n ^ s) & 0x8000u) ||
                        abs((int)(n & 31) - (int)(s & 31)) > 3 ||
                        abs((int)((n >> 5) & 31) - (int)((s >> 5) & 31)) > 3 ||
                        abs((int)((n >> 10) & 31) - (int)((s >> 10) & 31)) > 3) {
                        interior = 0;
                        break;
                    }
                }
            if (!interior) continue;
            in_total++;
            if (d == 0) in_exact++;
            if (d <= 2) in_ok++;
            if (((s >> 15) & 1) != (h[3] > 127)) in_mask++;
        }
    double it = in_total ? (double)in_total : 1.0;
    double pe = 100.0 * in_exact / it, pok = 100.0 * in_ok / it;
    double pm = 100.0 * in_mask / it;
    double pall = 100.0 * all_ok / ((double)W * Hh);
    int pass = pok >= c->min_ok && (S > 1 || pe >= c->min_exact) &&
               pm <= 100.0 - c->min_ok && pall >= 85.0;
    printf("  %dx %-22s interior %6.2f%% identical %6.2f%% within 2/31, mask %4.2f%% off;"
           " all pixels %6.2f%%  %s\n",
           S, c->name, pe, pok, pm, pall, pass ? "ok" : "MISMATCH");
    char name[96];
    snprintf(name, sizeof name, "selftest_out/hw_%s_%dx.png", c->name, S);
    for (char *q = name; *q; q++) if (*q == ' ') *q = '_';
    write_rgba_png(name, px, (unsigned)(W * S), (unsigned)(Hh * S));
    snprintf(name, sizeof name, "selftest_out/hw_%s_native.png", c->name);
    for (char *q = name; *q; q++) if (*q == ' ') *q = '_';
    write_sw_png(name, 0, 0, W, Hh);
    free(px);
    return pass;
}

static void st_gte(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ac3_gte_ctc2(&ctx, 0, 0x00001000); ac3_gte_ctc2(&ctx, 1, 0);
    ac3_gte_ctc2(&ctx, 2, 0x00001000); ac3_gte_ctc2(&ctx, 3, 0);
    ac3_gte_ctc2(&ctx, 4, 0x1000);
    ac3_gte_ctc2(&ctx, 5, 0); ac3_gte_ctc2(&ctx, 6, 0); ac3_gte_ctc2(&ctx, 7, 0);
    ac3_gte_ctc2(&ctx, 24, 160u << 16);
    ac3_gte_ctc2(&ctx, 25, 120u << 16);
    ac3_gte_ctc2(&ctx, 26, 256);
    ac3_gte_mtc2(&ctx, 0, (37u << 16) | 100u);
    ac3_gte_mtc2(&ctx, 1, 700);
    for (int ws = 0; ws < 2; ws++) {
        ac3_ws_factor = ws ? 0.75f : 1.0f;
        ac3_pgxp_enabled = 1;
        ac3_gte_op(&ctx, 0x00080001);
        u32 sxy = ac3_gte_mfc2(&ctx, 14);
        s16 ix = (s16)(sxy & 0xFFFF), iy = (s16)(sxy >> 16);
        double want_x = 160.0 + 100.0 * 256.0 / 700.0 * ac3_ws_factor;
        double want_y = 120.0 + 37.0 * 256.0 / 700.0;
        ac3_pgxp_vtx_t v;
        memset(&v, 0, sizeof v);
        int found = ac3_pgxp_lookup(sxy, &v);
        int ok = found && fabs(v.sx - want_x) < 0.01 && fabs(v.sy - want_y) < 0.01 &&
                 fabs(v.vz - 700.0) < 0.01 && abs(ix - (int)want_x) <= 1 &&
                 abs(iy - (int)want_y) <= 1;
        printf("  GTE %s: integer (%d,%d), precise (%.3f,%.3f) z %.1f, expected (%.3f,%.3f)  %s\n",
               ws ? "16:9" : "4:3 ", ix, iy, v.sx, v.sy, v.vz, want_x, want_y,
               ok ? "ok" : "MISMATCH");
        if (!ok) st_fail++;
    }
    ac3_ws_factor = 1.0f;
    ac3_pgxp_next_frame();
}

static void st_effect_order(void) {
    ac3_hd_settings_t saved = ac3_hd;
    int map_valid = hw_map.valid;
    memset(&ac3_hd, 0, sizeof ac3_hd);
    ac3_hd.fx = 1;
    hw_map.valid = 1;
    hw_scene_t scene = { 0 };
    scene.valid = 1; scene.H = 256; scene.ws = 1;
    scene.rot[0] = scene.rot[4] = scene.rot[8] = 1;
    scene.sun[1] = -1;
    for (int i = 0; i < HW_SKY_BINS; i++)
        for (int k = 0; k < 3; k++) scene.sky[i][k] = 0.2f;
    int S = hw_scale(), W = 320 * S, Hh = 240 * S;
    u8 *px = malloc((size_t)W * Hh * 4);
    int ok = px != NULL;
    for (int list = 0; list < 2 && ok; list++) {
        st_clear();
        glue_list_begin(&scene);
        glue_phase(PH_TERRAIN);
        gpu_poly_t p = { 0 };
        p.nverts = 4;
        p.x[0] = p.x[2] = 40; p.x[1] = p.x[3] = 200;
        p.y[0] = p.y[1] = 40; p.y[2] = p.y[3] = 90;
        for (int k = 0; k < 4; k++) p.r[k] = 120;
        p.semitrans = 1; p.tpage = 1 << 5;
        glue_poly(&gpu_sw, &p, NULL);
        gpu_rect_t r = { 0 };
        r.x = 40; r.y = 100; r.w = 160; r.h = 30;
        r.r = 120; r.semitrans = 1; r.tpage = 1 << 5;
        glue_rect(&gpu_sw, &r);
        gpu_line_t l = { 0 };
        l.nverts = 2; l.x[0] = 40; l.x[1] = 200;
        l.y[0] = l.y[1] = 140; l.r[0] = l.r[1] = 120;
        l.semitrans = 1; l.tpage = 1 << 5;
        glue_line(&gpu_sw, &l);
        if (!list) {
            p.semitrans = 0;
            p.x[0] = p.x[2] = 100; p.x[1] = p.x[3] = 180;
            p.y[0] = p.y[1] = 20; p.y[2] = p.y[3] = 180;
            for (int k = 0; k < 4; k++) { p.r[k] = 0; p.b[k] = 120; }
            glue_poly(&gpu_sw, &p, NULL);
        }
        r.x = 100; r.y = 155; r.w = 80; r.h = 10;
        glue_rect(&gpu_sw, &r);
        glue_phase(PH_HUD);
        glue_list_end();
        headless_flush();
        if (!hw_readback(HV.q, HV.pool, 0, 0, 320, 240, px)) { ok = 0; break; }
        const int ys[] = { 60, 115, 140 };
        for (int i = 0; i < 3; i++) {
            const u8 *visible = px + ((size_t)(ys[i] * S) * W + 60 * S) * 4;
            const u8 *covered = px + ((size_t)(ys[i] * S) * W + 120 * S) * 4;
            if (visible[0] < 100 || (list ? covered[0] < 100 : covered[0] > 8)) ok = 0;
        }
        const u8 *front = px + ((size_t)(160 * S) * W + 120 * S) * 4;
        if (front[0] < 100) ok = 0;
    }
    printf("  %dx deferred polygon/sprite/line occlusion and list reset  %s\n", S, ok ? "ok" : "MISMATCH");
    if (!ok) st_fail++;
    free(px);
    ac3_hd = saved;
    hw_map.valid = map_valid;
    hw_draw_order(0);
}

int ac3_selftest_hw(void) {
    static const st_case_t cases[] = {
        { "flat and gouraud",   scene_flat,     99.0, 80.0 },
        { "textured 4 8 15 bit", scene_textured, 97.0,  0.0 },
        { "semi-transparency",  scene_blend,    97.0,  0.0 },
        { "sprites",            scene_sprites,  99.5,  0.0 },
        { "mask bit",           scene_mask,     99.5, 98.0 },
        { "vram copies",        scene_copy,     99.0, 80.0 },
    };
    printf("enhanced renderer: precise vertices\n");
    st_gte();
    printf("enhanced renderer against the software GPU\n");
    _mkdir("selftest_out");
    int scales[2] = { 1, 4 };
    extern void ac3_vram_init(void);
    ac3_vram_init();
    ac3_hd.pgxp = 0;
    for (int si = 0; si < 2; si++) {
        memset(ac3_vram, 0, (size_t)GPU_VRAM_W * GPU_VRAM_H * 2);
        gpu_sw_reset();
        st_rng = 1234567u;
        if (!headless_init(scales[si])) { printf("  no Vulkan device\n"); return 1; }
        int S = hw_scale();
        gpu_sw_hooks = &hooks;
        st_textures();
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            st_rng = 777u + i;
            if (!st_run(&cases[i], S)) st_fail++;
        }
        st_effect_order();
        gpu_sw_hooks = NULL;
        headless_shutdown();
    }
    ac3_hd.pgxp = 1;
    printf("%s (%d failures)\n", st_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", st_fail);
    return st_fail ? 1 : 0;
}

#endif /* !AC3_HW_WEBGPU */