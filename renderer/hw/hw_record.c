/*
 * hw_record.c -- capture: records everything the hooks receive for a few
 * fields (hw_rec_arm, the capture key), with the VRAM and game RAM it started
 * from, into captures_hd/frame_NNNN.ac3r. The native build's --replay renders
 * a capture again (hw_replay.c), which is how the enhanced renderer is
 * debugged without playing to the same spot.
 */
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif
#include "hw_glue_internal.h"

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

int rec_active(void) {
    return rec.active;
}

/* every projection the GTE makes (the precise vertices) */
static void rec_gte(u32 key, const ac3_pgxp_vtx_t *v) {
    if (!rec.active) return;
    rec_head(R_GTE, 4 + sizeof *v);
    rec_put(&key, 4);
    rec_put(v, sizeof *v);
}

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

/* ---- one record per hook call (each does nothing unless recording) -------- */

void rec_poly(const gpu_poly_t *p, const hw_pv_t pv[4]) {
    if (!rec.active) return;
    rec_head(R_POLY, sizeof gpu_sw + sizeof *p + sizeof(hw_pv_t) * 4);
    rec_put(&gpu_sw, sizeof gpu_sw);
    rec_put(p, sizeof *p);
    rec_put(pv, sizeof(hw_pv_t) * 4);
}

void rec_rect(const gpu_rect_t *r) {
    if (!rec.active) return;
    rec_head(R_RECT, sizeof gpu_sw + sizeof *r);
    rec_put(&gpu_sw, sizeof gpu_sw);
    rec_put(r, sizeof *r);
}

void rec_line(const gpu_line_t *l) {
    if (!rec.active) return;
    rec_head(R_LINE, sizeof gpu_sw + sizeof *l);
    rec_put(&gpu_sw, sizeof gpu_sw);
    rec_put(l, sizeof *l);
}

void rec_fill(const PSX_RECT *r, u16 pixel) {
    if (!rec.active) return;
    rec_head(R_FILL, sizeof *r + 2);
    rec_put(r, sizeof *r);
    rec_put(&pixel, 2);
}

void rec_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    if (!rec.active) return;
    rec_head(R_COPY, sizeof gpu_sw + sizeof *src + 8);
    rec_put(&gpu_sw, sizeof gpu_sw);
    rec_put(src, sizeof *src);
    rec_put(&dx, 4);
    rec_put(&dy, 4);
}

/* the uploaded pixels, from VRAM (they are already there) */
void rec_upload(const PSX_RECT *r) {
    if (!rec.active || r->w <= 0 || r->h <= 0) return;
    u32 n = (u32)r->w * (u32)r->h;
    rec_head(R_UPLOAD, sizeof *r + n * 2);
    rec_put(r, sizeof *r);
    for (s32 y = 0; y < r->h; y++)
        for (s32 x = 0; x < r->w; x++)
            rec_put(vram_at((r->x + x) & 1023, (r->y + y) & 511), 2);
}

void rec_list(int begin, const hw_scene_t *cam) {
    if (!rec.active) return;
    u32 b = (u32)begin;
    rec_head(R_LIST, 4 + sizeof *cam);
    rec_put(&b, 4);
    rec_put(cam, sizeof *cam);
}

void rec_phase(int ph) {
    if (!rec.active) return;
    u32 p = (u32)ph;
    rec_head(R_PHASE, 4 + sizeof gpu_sw);
    rec_put(&p, 4);
    rec_put(&gpu_sw, sizeof gpu_sw);
}

/* End of a field: recorded, and the capture written after the last one; or,
 * if armed, the capture starts here. */
void rec_field(const gpu_display_t *d) {
    if (rec.active) {
        rec_head(R_FIELD, sizeof *d);
        rec_put(d, sizeof *d);
        if (--rec.fields_left <= 0) rec_finish();
    } else if (rec.armed) {
        rec_start();
    }
}
