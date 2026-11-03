/*
 * hw_glue_diag.c -- development aids for the glue: per-list statistics (the
 * replay tool prints them), --glue-trace (every drawing operation of a few
 * fields), and --sun-test / --cam-test (a fixed sun or camera). None of this
 * changes the picture unless one of those options is given.
 */
#include "hw_glue_internal.h"

hw_glue_stats_t gstat;
int glue_verbose;

/* world polygons by kind (glue_verbose only) */
static struct {
    int polys, all, none, mixed, semi, semi_precise, semi_ground, sealevel, lines, rects, rsemi;
    u32 keys[16]; int keyn[16];   /* textures of polygons at sea level */
} wd;

/* terrain depth from the map / depth from the game's projection */
static float diag[4096];
static int ndiag;

static int diag_cmp(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

void diag_world(const gpu_poly_t *p, const hw_pv_t *pv) {
    if (!glue_verbose) return;
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

/* The camera check: the map's depth along each precise vertex's ray against
 * the game's own depth (1.0 when the camera matches). */
void diag_depth(const gpu_poly_t *p, const hw_pv_t *pv) {
    if (!(glue_verbose && hw_map.valid && G.phase >= PH_TERRAIN && G.phase <= PH_OBJECTS &&
          p->textured && ndiag < 4096))
        return;
    for (int i = 0; i < (p->nverts == 4 ? 4 : 3); i++) {
        if (!pv[i].ok || pv[i].v.vz < 800.0f || pv[i].v.vz > 25000.0f) continue;
        float zr = hw_world_raycast(&G.scene, pv[i].v.sx, pv[i].v.sy, 50.0f, 80000.0f);
        if (zr > 0.0f && ndiag < 4096) diag[ndiag++] = zr / pv[i].v.vz;
    }
}

void diag_rect(const gpu_rect_t *r) {
    if (glue_verbose && G.flight && in_world()) { wd.rects++; if (r->semitrans) wd.rsemi++; }
}

void diag_line(void) {
    if (glue_verbose && G.flight && in_world()) wd.lines++;
}

/* End of a world list: prints the statistics (glue_verbose) and starts over. */
void diag_report(void) {
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

/* ---- --sun-test and --cam-test -------------------------------------------- */

float hw_sun_test[2];      /* screen position (or, with --cam-test, azimuth and elevation) */
int hw_sun_test_on;

/* puts the sun at a point on the screen */
void sun_test(hw_scene_t *s) {
    float d[3] = { hw_sun_test[0] * 160.0f / (s->H * s->ws), hw_sun_test[1] * 120.0f / s->H, 1.0f };
    float w[3], l = 0;
    for (int i = 0; i < 3; i++) {
        w[i] = s->rot[i] * d[0] + s->rot[3 + i] * d[1] + s->rot[6 + i] * d[2];
        l += w[i] * w[i];
    }
    l = sqrtf(l);
    for (int i = 0; i < 3; i++) s->sun[i] = w[i] / l;
}

float hw_cam_test[5];      /* x, y, z, yaw, pitch */
int hw_cam_test_on;
int hw_cam_sweep_n = 1;    /* --cam-sweep: replay passes, moving by hw_cam_sweep_d */
float hw_cam_sweep_d[5];

/* replaces the camera; with --sun-test too, the sun is at an azimuth and
 * elevation in degrees */
void cam_test(hw_scene_t *s) {
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
    if (hw_sun_test_on) {
        float az = hw_sun_test[0] * 0.0174533f, el = hw_sun_test[1] * 0.0174533f;
        s->sun[0] = sinf(az) * cosf(el);
        s->sun[1] = -sinf(el);
        s->sun[2] = cosf(az) * cosf(el);
    }
}

/* ---- --glue-trace N: every drawing operation of fields N to N+2 ----------- */

u32 hw_glue_trace_field;

int tracing(void) {
    return hw_glue_trace_field && G.fields >= hw_glue_trace_field && G.fields < hw_glue_trace_field + 3;
}

static void trace_box(const char *what, const s16 *x, const s16 *y, int n, const char *extra) {
    if (!tracing()) return;
    int x0 = 99999, y0 = 99999, x1 = -99999, y1 = -99999;
    for (int i = 0; i < n; i++) {
        if (x[i] < x0) x0 = x[i];
        if (x[i] > x1) x1 = x[i];
        if (y[i] < y0) y0 = y[i];
        if (y[i] > y1) y1 = y[i];
    }
    printf("trace f%u ph%d area(%d,%d-%d,%d) ofs(%d,%d) %s box %d,%d-%d,%d %s\n", G.fields, G.phase,
           gpu_sw.area_x0, gpu_sw.area_y0, gpu_sw.area_x1, gpu_sw.area_y1, gpu_sw.ofs_x, gpu_sw.ofs_y,
           what, x0, y0, x1, y1, extra);
}

void trace_poly(const gpu_poly_t *p) {
    if (!tracing()) return;
    char e[96];
    snprintf(e, sizeof e, "%s%s rgb %02x%02x%02x tpage %04x clut %04x", p->textured ? "tex" : "flat", p->semitrans ? " semi" : "",
             p->r[0], p->g[0], p->b[0], p->tpage, p->clut);
    trace_box("poly", p->x, p->y, p->nverts == 4 ? 4 : 3, e);
}

void trace_rect(const gpu_rect_t *r) {
    if (!tracing()) return;
    s16 x[2] = { (s16)r->x, (s16)(r->x + r->w - 1) }, y[2] = { (s16)r->y, (s16)(r->y + r->h - 1) };
    char e[96];
    snprintf(e, sizeof e, "%s%s rgb %02x%02x%02x tpage %04x clut %04x", r->textured ? "tex" : "flat", r->semitrans ? " semi" : "",
             r->r, r->g, r->b, r->tpage, r->clut);
    trace_box("rect", x, y, 2, e);
}

void trace_fill(const PSX_RECT *r, u16 pixel) {
    if (!tracing()) return;
    s16 x[2] = { r->x, (s16)(r->x + r->w - 1) }, y[2] = { r->y, (s16)(r->y + r->h - 1) };
    char e[32];
    snprintf(e, sizeof e, "pixel %04x", pixel);
    trace_box("FILL", x, y, 2, e);
}

void trace_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    if (!tracing()) return;
    s16 x[2] = { (s16)dx, (s16)(dx + src->w - 1) }, y[2] = { (s16)dy, (s16)(dy + src->h - 1) };
    char e[48];
    snprintf(e, sizeof e, "from %d,%d", src->x, src->y);
    trace_box("COPY", x, y, 2, e);
}

void trace_upload(const PSX_RECT *r) {
    if (!tracing()) return;
    s16 x[2] = { r->x, (s16)(r->x + r->w - 1) }, y[2] = { r->y, (s16)(r->y + r->h - 1) };
    trace_box("UPLOAD", x, y, 2, "");
}
