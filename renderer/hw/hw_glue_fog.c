/*
 * hw_glue_fog.c -- the game's fog, as the enhanced renderer sees it.
 *
 * Fog bands. The game fades its colour tables (CLUTs) with distance: while it
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
 * whatever colour that spot had (green when it was the HUD).
 *
 * Original fog (ac3_hd.game_fog): the game's own fade is kept on the ground it
 * draws (no band replacement), and the far terrain gets the same fade. Its
 * distances are measured from the game: ground drawn with the band table
 * before the last band copy is faded; the second-to-last band starts the fade
 * and the farthest faded ground ends it (about 15,000-19,000 to 30,000 units,
 * depending on the mission).
 */
#include "hw_glue_internal.h"

/* the series seen this frame, and last frame's (each at its nearest table) */
static band_t band_cur[MAX_BANDS];
static int nband_cur;
band_t band_near[MAX_BANDS];
int nband_near;

/* the main (largest) series, measured for the original fog */
static struct {
    s32 dx, dy, w, h;      /* the main band's destination (0 size: none) */
    int k, n_prev;         /* band copies so far this frame; last frame's total */
    float lo, hi;          /* this frame's fade start and end samples */
    float fs, fe;          /* smoothed */
    PSX_RECT first, last;  /* this frame's farthest and nearest table */
    float col[3];          /* the colour the game fades to */
    int have, have_col;
} FOG = { .lo = 1e30f };

/* ---- the original fog's colour and distances ------------------------------ */

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

/* At the start of a list: the main series is last frame's largest, and the
 * scene gets the fade measured so far (original fog only). */
void fog_list_begin(hw_scene_t *s) {
    FOG.w = FOG.h = 0;
    for (int i = 0; i < nband_near; i++)
        if (band_near[i].src.w * band_near[i].src.h > FOG.w * FOG.h) {
            FOG.dx = band_near[i].dx; FOG.dy = band_near[i].dy;
            FOG.w = band_near[i].src.w; FOG.h = band_near[i].src.h;
        }
    s->fog_z[0] = s->fog_z[1] = 0.0f;
    if (ac3_hd.game_fog && FOG.have && FOG.have_col) {
        s->fog_z[0] = FOG.fs; s->fog_z[1] = FOG.fe;
        memcpy(s->game_fog_col, FOG.col, sizeof FOG.col);
    }
}

/* A world polygon: if it uses the main series' table, its depths say how far
 * the fade reaches. */
void fog_sample(const gpu_poly_t *p, const hw_pv_t *pv) {
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

/* A band copy: counts the main series' copies and keeps its first and last
 * table (for the colour). */
void fog_note_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    if (FOG.w && dx == FOG.dx && dy == FOG.dy && src->w == FOG.w && src->h == FOG.h) {
        if (!FOG.k) FOG.first = *src;
        FOG.last = *src;
        FOG.k++;
    }
}

void fog_frame_end(void) {
    if (FOG.hi > 0.0f && FOG.lo < 1e29f && FOG.hi > FOG.lo + 1000.0f) {
        if (!FOG.have) { FOG.fs = FOG.lo; FOG.fe = FOG.hi; FOG.have = 1; }
        else { FOG.fs += (FOG.lo - FOG.fs) * 0.1f; FOG.fe += (FOG.hi - FOG.fe) * 0.1f; }
    }
    if (FOG.k >= 2) fog_colour();
    FOG.n_prev = FOG.k;
    FOG.k = 0;
    FOG.lo = 1e30f; FOG.hi = 0.0f;
}

/* ---- band series ---------------------------------------------------------- */

void bands_restart(void) {
    nband_cur = 0;
}

/* End of a frame: the series seen at least 3 times become the known ones
 * (keep 0: none are known). */
void bands_settle(int keep) {
    nband_near = 0;
    if (keep)
        for (int i = 0; i < nband_cur; i++)
            if (band_cur[i].n >= 3) band_near[nband_near++] = band_cur[i];
}

/* a copy from outside the drawing area (not from the picture itself) */
int band_candidate(const gpu_sw_state_t *st, const PSX_RECT *src) {
    return !(src->x <= st->area_x1 && src->x + src->w > st->area_x0 &&
             src->y <= st->area_y1 && src->y + src->h > st->area_y0);
}

static int band_same(const band_t *b, const PSX_RECT *src, s32 dx, s32 dy) {
    return b->dx == dx && b->dy == dy && b->src.w == src->w && b->src.h == src->h;
}

void band_note(const PSX_RECT *src, s32 dx, s32 dy) {
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

/* the nearest table of this copy's series, if the series is known */
const PSX_RECT *band_nearest(const PSX_RECT *src, s32 dx, s32 dy) {
    for (int i = 0; i < nband_near; i++)
        if (band_same(&band_near[i], src, dx, dy)) return &band_near[i].src;
    return NULL;
}

const band_t *band_largest(void) {
    const band_t *pb = NULL;
    for (int i = 0; i < nband_near; i++)
        if (!pb || band_near[i].src.w * band_near[i].src.h > pb->src.w * pb->src.h)
            pb = &band_near[i];
    return pb;
}

/* Puts every known series' nearest table in place at once, smallest first
 * (so a larger series covering a smaller one wins). */
void bands_copy_nearest(void) {
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
