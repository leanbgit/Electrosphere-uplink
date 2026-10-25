/*
 * hw_ocean_web.c -- the CPU parts of renderer/hw/hw_ocean.c for the browser
 * build, copied unchanged: the wave spectrum (random seed, cascades, slope
 * table) and the choice of visible water patches. Built into both workers:
 * the render worker needs the spectrum (initial waves), the game worker the
 * parameters and the patch lists. The GPU half is in hw_ocean_webgpu.c.
 * Keep in step with hw_ocean.c.
 */
#include "hw_ocean_web.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ON HW_OCEAN_N
#define NC HW_OCEAN_CASCADES

static const float PATCH_M[NC] = { 500.0f, 85.0f, 17.0f };
static const float KBAND[NC + 1] = { 0.0f, 0.296f, 1.48f, 20.0f };
static const float ROT_DEG[NC] = { 0.0f, 8.0f, -13.0f };
static const float WIND_DIR[2] = { 0.8253f, 0.5646f };
#define WIND_MS     10.0
#define SIG_HEIGHT  2.0
#define CHOPPY      1.2f
#define REPEAT_S    200.0
#define FOAM_LIFE_S 2.0
#define UNITS_PER_M 5.0f

#define LODS  18
#define CELL0 1.5
#define KRANGE 4.0
#define EARTH_INV_2R (1.0 / (2.0 * 6.371e6 * 5.0))

_Static_assert(2 + (NC * HW_OCEAN_MIPS + 3) / 4 <= HW_OCEAN_PARAMS,
               "the roughness table does not fit hw_fx_common.glsl's ocean[]");


static struct { float params[HW_OCEAN_PARAMS][4]; } O;

static u32 rng_state;
static double rnd01(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return ((double)(rng_state >> 8) + 0.5) / 16777216.0;
}
static void gauss2(double *a, double *b) {
    double u = rnd01(), v = rnd01();
    double r = sqrt(-2.0 * log(u));
    *a = r * cos(6.283185307179586 * v);
    *b = r * sin(6.283185307179586 * v);
}

static void make_spectrum(float *out) {   /* out may be NULL: parameters only */
    const double g = 9.81, Lw = WIND_MS * WIND_MS / g;
    static double re[NC][ON * ON], im[NC][ON * ON], kk[NC][ON * ON];
    double var = 0.0;
    for (int c = 0; c < NC; c++) {
        double a = ROT_DEG[c] * 3.14159265358979 / 180.0;
        double wx = cos(a) * WIND_DIR[0] - sin(a) * WIND_DIR[1];
        double wz = sin(a) * WIND_DIR[0] + cos(a) * WIND_DIR[1];
        double dk = 6.283185307179586 / PATCH_M[c];
        rng_state = 0x9E3779B9u * (u32)(c + 1) + 12345u;
        for (int j = 0; j < ON; j++)
            for (int i = 0; i < ON; i++) {
                int o = j * ON + i;
                double kx = dk * (i - ON / 2), kz = dk * (j - ON / 2);
                double k = sqrt(kx * kx + kz * kz);
                double ga, gb;
                gauss2(&ga, &gb);
                re[c][o] = im[c][o] = 0.0;
                kk[c][o] = k;
                if (i == 0 || j == 0 || k <= 0.0 || k < KBAND[c] || k >= KBAND[c + 1]) continue;
                double d = (kx * wx + kz * wz) / k;
                double P = exp(-1.0 / (k * Lw * k * Lw)) / (k * k * k * k) * d * d *
                           exp(-(k * 0.04) * (k * 0.04));
                if (d < 0.0) P *= 0.07;
                double amp = sqrt(P * dk * dk * 0.5);
                re[c][o] = amp * ga;
                im[c][o] = amp * gb;
                var += 2.0 * (re[c][o] * re[c][o] + im[c][o] * im[c][o]);
            }
    }
    double scale = var > 0.0 ? (SIG_HEIGHT / 4.0) / sqrt(var) : 0.0;
    double slope_total = 0.0;
    float lost[NC][HW_OCEAN_MIPS];
    for (int c = 0; c < NC; c++) {
        double band_lost[HW_OCEAN_MIPS] = { 0 };
        for (int o = 0; o < ON * ON; o++) {
            re[c][o] *= scale;
            im[c][o] *= scale;
        }
        for (int j = 0; j < ON; j++)
            for (int i = 0; i < ON; i++) {
                int o = j * ON + i;
                int n = ((ON - j) & (ON - 1)) * ON + ((ON - i) & (ON - 1));
                if (out) {
                    float *t = out + ((size_t)c * ON * ON + (size_t)o) * 4;
                    t[0] = (float)re[c][o];
                    t[1] = (float)im[c][o];
                    t[2] = (float)re[c][n];
                    t[3] = (float)-im[c][n];
                }
                double s = 2.0 * kk[c][o] * kk[c][o] * (re[c][o] * re[c][o] + im[c][o] * im[c][o]);
                slope_total += s;
                for (int m = 0; m < HW_OCEAN_MIPS; m++) {
                    double kcut = 0.6 * 3.14159265358979 * ON / (PATCH_M[c] * (double)(1 << m));
                    if (kk[c][o] > kcut) band_lost[m] += s;
                }
            }
        for (int m = 0; m < HW_OCEAN_MIPS; m++) lost[c][m] = (float)band_lost[m];
    }
    double cox = 0.003 + 0.00512 * WIND_MS;
    double base = cox - slope_total;
    if (base < 0.006) base = 0.006;
    memset(O.params, 0, sizeof O.params);
    for (int c = 0; c < NC; c++) O.params[0][c] = PATCH_M[c] * UNITS_PER_M;
    O.params[0][3] = CHOPPY;
    O.params[1][0] = (float)base;
    O.params[1][1] = UNITS_PER_M;
    for (int c = 0; c < NC; c++)
        for (int m = 0; m < HW_OCEAN_MIPS; m++) {
            int k = c * HW_OCEAN_MIPS + m;
            O.params[2 + k / 4][k % 4] = lost[c][m];
        }
    if (out) printf("ocean: Hs %.1f m, slope variance %.4f in the waves, %.4f below them\n",
           SIG_HEIGHT, slope_total, base);
    if (getenv("AC3_OCEAN_DEBUG"))
        for (int c = 0; c < NC; c++)
            printf("  cascade %d lost: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n", c,
                   lost[c][0], lost[c][1], lost[c][2], lost[c][3], lost[c][4], lost[c][5],
                   lost[c][6], lost[c][7], lost[c][8]);
}


void hw_ocean_spectrum(float *h0, float params[HW_OCEAN_PARAMS][4]) {
    make_spectrum(h0);
    if (params) memcpy(params, O.params, sizeof O.params);
}

typedef struct {
    double cx, cy, cz, alt, sea_y, horizon;
    float rot[9];
    float hw, hh, nx, ny;
    hw_ocean_node_t *out;
    u32 n, max;
} sel_t;

static double node_size(int L) { return 32.0 * CELL0 * ldexp(1.0, L); }
static double node_range(int L) { return KRANGE * node_size(L); }

static double rect_h2(const sel_t *s, double x0, double z0, double size) {
    double dx = fabs(s->cx - (x0 + size * 0.5)) - size * 0.5;
    double dz = fabs(s->cz - (z0 + size * 0.5)) - size * 0.5;
    if (dx < 0) dx = 0;
    if (dz < 0) dz = 0;
    return dx * dx + dz * dz;
}

static int visible(const sel_t *s, double x0, double z0, double size) {
    double half = size * 0.5;
    double hx = x0 + half - s->cx, hz = z0 + half - s->cz;
    double D = sqrt(hx * hx + hz * hz);
    double R = half * 1.4143 + 80.0 + EARTH_INV_2R * (2.0 * D * half * 1.4143 + 2.0 * half * half);
    double rel[3] = { hx, s->sea_y + D * D * EARTH_INV_2R - s->cy, hz };
    double v[3];
    for (int i = 0; i < 3; i++)
        v[i] = s->rot[3 * i] * rel[0] + s->rot[3 * i + 1] * rel[1] + s->rot[3 * i + 2] * rel[2];
    if (v[2] < -R) return 0;
    if ((v[0] - s->hw * v[2]) * s->nx > R || (-v[0] - s->hw * v[2]) * s->nx > R) return 0;
    if ((v[1] - s->hh * v[2]) * s->ny > R || (-v[1] - s->hh * v[2]) * s->ny > R) return 0;
    return 1;
}

static void emit(sel_t *s, double x0, double z0, double cell, double range) {
    if (s->n >= s->max) return;
    if (rect_h2(s, x0, z0, cell * HW_OCEAN_QUADS) > s->horizon * s->horizon) return;
    if (!visible(s, x0, z0, cell * HW_OCEAN_QUADS)) return;
    hw_ocean_node_t *o = &s->out[s->n++];
    o->x = (float)x0; o->z = (float)z0; o->cell = (float)cell; o->range = (float)range;
}

static int select_node(sel_t *s, int L, double x0, double z0) {
    double size = node_size(L), r = node_range(L);
    double h2 = rect_h2(s, x0, z0, size);
    double d2 = h2 + s->alt * s->alt;
    if (d2 >= r * r) return 0;
    if (h2 > s->horizon * s->horizon || !visible(s, x0, z0, size)) return 1;
    double q = size * 0.5, cell = size / (2.0 * HW_OCEAN_QUADS);
    double rc = L > 0 ? node_range(L - 1) : 0.0;
    int split = L > 0 && d2 < rc * rc;
    for (int i = 0; i < 4; i++) {
        double qx = x0 + (i & 1) * q, qz = z0 + (i >> 1) * q;
        if (!split || !select_node(s, L - 1, qx, qz)) emit(s, qx, qz, cell, r);
    }
    return 1;
}

u32 hw_ocean_select(const hw_scene_t *sc, float horizon, hw_ocean_node_t *out, u32 max) {
    sel_t s;
    memset(&s, 0, sizeof s);
    s.cx = sc->cam[0]; s.cy = sc->cam[1]; s.cz = sc->cam[2];
    s.sea_y = sc->sea_y;
    s.alt = s.sea_y - s.cy;
    if (s.alt < 1.0) s.alt = 1.0;
    s.horizon = horizon;
    memcpy(s.rot, sc->rot, sizeof s.rot);
    s.hw = (float)(sc->area[2] - sc->area[0] + 1) * 0.5f * 1.15f / (sc->H * sc->ws);
    s.hh = (float)(sc->area[3] - sc->area[1] + 1) * 0.5f * 1.15f / sc->H;
    s.nx = 1.0f / sqrtf(1.0f + s.hw * s.hw);
    s.ny = 1.0f / sqrtf(1.0f + s.hh * s.hh);
    s.out = out;
    s.max = max;
    int top = LODS - 1;
    double size = node_size(top);
    double bx = floor(s.cx / size) * size, bz = floor(s.cz / size) * size;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            select_node(&s, top, bx + i * size, bz + j * size);
    return s.n;
}
