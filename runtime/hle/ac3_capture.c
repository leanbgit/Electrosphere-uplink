#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

extern u32 ac3_vblank_count(void);
extern void ac3_render_screenshot(const char *path);

#define LOG_LINES 24000
#define LOG_WIDTH 160
static char log_ring[LOG_LINES][LOG_WIDTH];
static u32  log_frame[LOG_LINES];
static unsigned log_head, log_count;

void ac3_capture_log(const char *line) {
    snprintf(log_ring[log_head], LOG_WIDTH, "%s", line);
    log_frame[log_head] = ac3_vblank_count();
    log_head = (log_head + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;
}

enum { PRIM_POLY, PRIM_RECT, PRIM_LINE, PRIM_LOAD };

typedef struct {
    u8  kind, nverts, flags;
    s16 x0, y0, x1, y1;
    s16 vx[4], vy[4];
    s16 ofx, ofy;
    u8  r, g, b;
    u8  u, v;
    u16 clut, tpage;
    u16 twin_mask, twin_off;
    u32 src;
} prim_rec_t;

#define MAX_PRIMS 8192
static prim_rec_t prims[MAX_PRIMS];
static unsigned nprims, prims_dropped;
static unsigned frame_polys, frame_rects, frame_lines, frame_loads;

static prim_rec_t *prim_new(u8 kind) {
    if (nprims >= MAX_PRIMS) { prims_dropped++; return NULL; }
    prim_rec_t *p = &prims[nprims++];
    memset(p, 0, sizeof *p);
    p->kind = kind;
    return p;
}

void ac3_capture_poly(const gpu_poly_t *g) {
    frame_polys++;
    prim_rec_t *p = prim_new(PRIM_POLY);
    if (!p) return;
    p->nverts = (u8)g->nverts;
    p->flags = (u8)((g->textured ? 1 : 0) | (g->semitrans ? 2 : 0) |
                    (g->gouraud ? 4 : 0) | (g->raw ? 8 : 0));
    ac3_gpu_current_offset(&p->ofx, &p->ofy);
    for (int i = 0; i < g->nverts && i < 4; i++) {
        p->vx[i] = g->x[i];
        p->vy[i] = g->y[i];
    }
    p->x0 = p->x1 = g->x[0];
    p->y0 = p->y1 = g->y[0];
    for (int i = 1; i < g->nverts; i++) {
        if (g->x[i] < p->x0) p->x0 = g->x[i];
        if (g->x[i] > p->x1) p->x1 = g->x[i];
        if (g->y[i] < p->y0) p->y0 = g->y[i];
        if (g->y[i] > p->y1) p->y1 = g->y[i];
    }
    p->r = g->r[0]; p->g = g->g[0]; p->b = g->b[0];
    p->u = g->u[0]; p->v = g->v[0];
    p->clut = g->clut; p->tpage = g->tpage;
    p->twin_mask = g->twin_mask; p->twin_off = g->twin_off;
}

void ac3_capture_rect(const gpu_rect_t *g) {
    frame_rects++;
    prim_rec_t *p = prim_new(PRIM_RECT);
    if (!p) return;
    p->nverts = 4;
    p->flags = (u8)((g->textured ? 1 : 0) | (g->semitrans ? 2 : 0) |
                    (g->raw ? 8 : 0));
    p->x0 = g->x; p->y0 = g->y;
    p->x1 = (s16)(g->x + g->w); p->y1 = (s16)(g->y + g->h);
    p->r = g->r; p->g = g->g; p->b = g->b;
    p->u = g->u; p->v = g->v;
    p->clut = g->clut; p->tpage = g->tpage;
    p->twin_mask = g->twin_mask; p->twin_off = g->twin_off;
}

void ac3_capture_line(const gpu_line_t *g) {
    frame_lines++;
    prim_rec_t *p = prim_new(PRIM_LINE);
    if (!p) return;
    p->nverts = (u8)(g->nverts > 255 ? 255 : g->nverts);
    p->flags = (u8)((g->semitrans ? 2 : 0) | (g->gouraud ? 4 : 0));
    p->x0 = p->x1 = g->x[0];
    p->y0 = p->y1 = g->y[0];
    for (int i = 1; i < g->nverts; i++) {
        if (g->x[i] < p->x0) p->x0 = g->x[i];
        if (g->x[i] > p->x1) p->x1 = g->x[i];
        if (g->y[i] < p->y0) p->y0 = g->y[i];
        if (g->y[i] > p->y1) p->y1 = g->y[i];
    }
    p->r = g->r[0]; p->g = g->g[0]; p->b = g->b[0];
    p->tpage = g->tpage;
}

#define LOAD_HIST 512
enum { VT_LOAD = 0, VT_COPY, VT_COPY_SKIPPED, VT_FILL };
typedef struct {
    u32 frame, src; s16 x, y, w, h; s16 dx, dy; u8 kind;
    u8 r, g, b;
    const char *who;
} load_rec_t;
static load_rec_t load_hist[LOAD_HIST];
static unsigned load_head, load_count, load_total;

void ac3_capture_load_image(const PSX_RECT *r, u32 src) {
    ac3_probe_load_image(r->x, r->y, r->w, r->h, src);
    frame_loads++;
    load_total++;
    load_rec_t *lr = &load_hist[load_head];
    load_head = (load_head + 1) % LOAD_HIST;
    if (load_count < LOAD_HIST) load_count++;
    lr->frame = ac3_vblank_count();
    lr->src = src;
    lr->x = r->x; lr->y = r->y; lr->w = r->w; lr->h = r->h;
    lr->kind = VT_LOAD;

    prim_rec_t *p = prim_new(PRIM_LOAD);
    if (!p) return;
    p->x0 = r->x; p->y0 = r->y;
    p->x1 = (s16)(r->x + r->w); p->y1 = (s16)(r->y + r->h);
    p->src = src;
}

void ac3_capture_vram_copy(const PSX_RECT *r, s16 dx, s16 dy, int skipped) {
    if (!r) return;
    load_rec_t *lr = &load_hist[load_head];
    load_head = (load_head + 1) % LOAD_HIST;
    if (load_count < LOAD_HIST) load_count++;
    lr->frame = ac3_vblank_count();
    lr->src = 0;
    lr->x = r->x; lr->y = r->y; lr->w = r->w; lr->h = r->h;
    lr->dx = dx; lr->dy = dy;
    lr->kind = (u8)(skipped ? VT_COPY_SKIPPED : VT_COPY);
}

#define MDEC_EVENTS 512
typedef struct {
    u32 frame;
    u8  kind;
    u32 addr, words, a, b, c;
} mdec_ev_t;
static mdec_ev_t mdec_ev[MDEC_EVENTS];
static unsigned mdec_head, mdec_count;

static mdec_ev_t *mdec_new(u8 kind) {
    mdec_ev_t *e = &mdec_ev[mdec_head];
    mdec_head = (mdec_head + 1) % MDEC_EVENTS;
    if (mdec_count < MDEC_EVENTS) mdec_count++;
    memset(e, 0, sizeof *e);
    e->frame = ac3_vblank_count();
    e->kind = kind;
    return e;
}

static u32 mdec_in_total, mdec_out_total;
static int pending, arm_on_video;
static int pending_needs_draw, pending_waited;

static unsigned frame_clears;
static PSX_RECT last_clear;
static u8 last_clear_rgb[3];
static const char *last_clear_who = "none";

void ac3_capture_fill_rect(const PSX_RECT *r, u8 red, u8 green, u8 blue,
                           const char *who) {
    frame_clears++;
    if (r) last_clear = *r;
    last_clear_rgb[0] = red; last_clear_rgb[1] = green; last_clear_rgb[2] = blue;
    last_clear_who = who ? who : "?";

    if (!r) return;
    load_rec_t *lr = &load_hist[load_head];
    load_head = (load_head + 1) % LOAD_HIST;
    if (load_count < LOAD_HIST) load_count++;
    memset(lr, 0, sizeof *lr);
    lr->frame = ac3_vblank_count();
    lr->x = r->x; lr->y = r->y; lr->w = r->w; lr->h = r->h;
    lr->r = red; lr->g = green; lr->b = blue;
    lr->who = who ? who : "?";
    lr->kind = VT_FILL;
}

void ac3_capture_on_video(void) {
    arm_on_video = 1;
    printf("\n*** capture armed: will fire on the next decoded video frame\n\n");
    fflush(stdout);
}

void ac3_capture_mdec_in(u32 buf, u32 cmd, u32 words, int bpp24) {
    mdec_in_total++;
    mdec_ev_t *e = mdec_new(0);
    e->addr = buf; e->words = words; e->a = cmd; e->b = (u32)bpp24;
    if (arm_on_video) { arm_on_video = 0; pending = 1; }
}

void ac3_capture_mdec_out(u32 dst, u32 words, u32 mb_done, u32 mb_want,
                          u32 bytes_in) {
    mdec_out_total++;
    mdec_ev_t *e = mdec_new(1);
    e->addr = dst; e->words = words;
    e->a = mb_done; e->b = mb_want; e->c = bytes_in;
}

#define HIST_FRAMES 1200
typedef struct {
    u32 frame;
    u32 stream_state, ring_fill;
    u8  gmv[8];
    u32 scene[4];
    u32 cd_lba;
    s32 cd_credit;
    u8  cd_streaming, cd_mode, filt_file, filt_chan;
    u32 mdec_in, mdec_out;
    u16 polys, rects, lines, loads;
    u16 clears;
} hist_row_t;
static hist_row_t hist[HIST_FRAMES];
static unsigned hist_head, hist_count;

static u32 crc_table[256];
static int crc_ready;

static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static u32 crc32_buf(u32 crc, const u8 *b, size_t n) {
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static void png_chunk(FILE *f, const char *type, const u8 *data, size_t len) {
    u8 hdr[8];
    put_be32(hdr, (u32)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    u32 crc = crc32_buf(0, hdr + 4, 4);
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    crc ^= 0xFFFFFFFFu;
    u8 c[4];
    put_be32(c, crc);
    fwrite(c, 1, 4, f);
}

int ac3_write_png_rgb(const char *path, unsigned w, unsigned h,
                      const u8 *rgb) {
    if (!w || !h) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    static const u8 sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    u8 ihdr[13];
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    u8 *raw = (u8 *)malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (unsigned y = 0; y < h; y++) {
        u8 *row = raw + (size_t)y * (1 + (size_t)w * 3);
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }

    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + blocks * 5 + raw_len + 4;
    u8 *z = (u8 *)malloc(z_len);
    if (!z) { free(raw); fclose(f); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off;
        if (n > 65535) n = 65535;
        z[zi++] = (off + n >= raw_len) ? 1 : 0;
        z[zi++] = (u8)(n & 0xFF);
        z[zi++] = (u8)(n >> 8);
        z[zi++] = (u8)(~n & 0xFF);
        z[zi++] = (u8)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }
    u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z + zi, (b << 16) | a);
    zi += 4;

    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(raw);
    return 1;
}

static void rgb555_to_rgb888(u16 px, u8 *out) {
    u32 r = (px & 0x1F), g = (px >> 5) & 0x1F, bl = (px >> 10) & 0x1F;
    out[0] = (u8)((r << 3) | (r >> 2));
    out[1] = (u8)((g << 3) | (g >> 2));
    out[2] = (u8)((bl << 3) | (bl >> 2));
}

static unsigned capture_seq;

static int guest_ok(u32 a) {
    u32 p = ac3_phys(a);
    return a != 0 && p + 64u < AC3_RAM_SIZE;
}
static u32 rd32(u32 a) { return guest_ok(a) ? MEM_R32(a) : 0; }
static u32 rd8(u32 a)  { return guest_ok(a) ? MEM_R8U(a) : 0; }

void ac3_capture_request(void) {
    pending = 1;
    pending_needs_draw = 1;
    pending_waited = 0;
}

static void dir_make(const char *p) { MKDIR(p); }

static void write_vram(const char *dir) {
    if (!ac3_vram) return;
    char path[512];
    snprintf(path, sizeof path, "%s/vram.bin", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(ac3_vram, 2, (size_t)GPU_VRAM_W * GPU_VRAM_H, f);
        fclose(f);
    }
    u8 *rgb = (u8 *)malloc((size_t)GPU_VRAM_W * GPU_VRAM_H * 3);
    if (!rgb) return;
    for (int y = 0; y < GPU_VRAM_H; y++)
        for (int x = 0; x < GPU_VRAM_W; x++)
            rgb555_to_rgb888(*vram_at(x, y),
                             rgb + ((size_t)y * GPU_VRAM_W + x) * 3);
    snprintf(path, sizeof path, "%s/vram.png", dir);
    ac3_write_png_rgb(path, GPU_VRAM_W, GPU_VRAM_H, rgb);
    free(rgb);
}

static void write_ram(const char *dir) {
    if (!ac3_ram) return;
    char path[512];
    snprintf(path, sizeof path, "%s/ram.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(ac3_ram, 1, AC3_RAM_SIZE, f);
    fclose(f);
}

static void write_mdec_pixels(const char *dir) {
    u32 len = 0, strip = 0;
    int bpp24 = 0;
    const u8 *px = ac3_mdec_last_pixels(&len, &bpp24, &strip);
    if (!px || !len) return;

    u32 mb_bytes = 16u * 16u * (bpp24 ? 3u : 2u);
    u32 mbs = len / mb_bytes;
    if (!mbs) return;

    u32 rows = (strip && strip <= mbs) ? strip : (mbs < 16 ? mbs : 16);
    u32 cols = (mbs + rows - 1) / rows;
    u32 w = cols * 16, h = rows * 16;

    u8 *rgb = (u8 *)calloc((size_t)w * h, 3);
    if (!rgb) return;
    for (u32 mb = 0; mb < mbs; mb++) {
        u32 tx = (mb / rows) * 16, ty = (mb % rows) * 16;
        for (u32 y = 0; y < 16; y++)
            for (u32 x = 0; x < 16; x++) {
                const u8 *s = px + (size_t)(mb * 256 + y * 16 + x) * (bpp24 ? 3 : 2);
                u8 *d = rgb + (((size_t)(ty + y) * w) + tx + x) * 3;
                if (bpp24) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
                else rgb555_to_rgb888((u16)(s[0] | (s[1] << 8)), d);
            }
    }
    char path[512];
    snprintf(path, sizeof path, "%s/mdec_out.png", dir);
    ac3_write_png_rgb(path, w, h, rgb);
    free(rgb);
}

static void write_log(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/trace.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# library trace leading up to the capture, oldest first\n");
    unsigned start = (log_count < LOG_LINES)
                     ? 0 : log_head;
    for (unsigned i = 0; i < log_count; i++) {
        unsigned k = (start + i) % LOG_LINES;
        fprintf(f, "[f%6u] %s\n", log_frame[k], log_ring[k]);
    }
    fclose(f);
}

static const char *tpage_depth(u16 tpage) {
    switch ((tpage >> 7) & 3) {
    case 0: return "4bit";
    case 1: return "8bit";
    case 2: return "15bit";
    default: return "res";
    }
}

static void write_prims(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/drawlist.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# every primitive submitted for the captured frame, in order.\n"
               "# tex-src is where a textured primitive samples VRAM from: the\n"
               "# texture page base plus the u,v of its first vertex.\n\n");
    fprintf(f, "%-5s %-5s %-24s %-11s %-9s %s\n",
            "idx", "kind", "screen rect", "colour", "flags", "texture");
    for (unsigned i = 0; i < nprims; i++) {
        const prim_rec_t *p = &prims[i];
        char rect[40], col[24], flags[24], tex[160];
        snprintf(rect, sizeof rect, "%5d,%-5d %5d,%-5d",
                 p->x0, p->y0, p->x1, p->y1);
        snprintf(col, sizeof col, "%3u,%3u,%3u", p->r, p->g, p->b);
        snprintf(flags, sizeof flags, "%s%s%s%s",
                 (p->flags & 1) ? "T" : "-", (p->flags & 2) ? "S" : "-",
                 (p->flags & 4) ? "G" : "-", (p->flags & 8) ? "R" : "-");
        tex[0] = 0;
        if (p->kind == PRIM_LOAD)
            snprintf(tex, sizeof tex, "from guest 0x%08X", p->src);
        else if (p->flags & 1) {
            int bx = (p->tpage & 0xF) * 64;
            int by = ((p->tpage >> 4) & 1) * 256;
            int cx = (p->clut & 0x3F) * 16;
            int cy = (p->clut >> 6) & 0x1FF;
            int n = snprintf(tex, sizeof tex,
                     "tpage=0x%03X %s base=(%d,%d) uv=(%u,%u) clut=(%d,%d)",
                     p->tpage, tpage_depth(p->tpage), bx, by, p->u, p->v, cx, cy);
            if (p->twin_mask)
                snprintf(tex + n, sizeof tex - (size_t)n,
                         " twin=(%u,%u)-(%u,%u)",
                         p->twin_off & 0xFFu, (p->twin_off >> 8) & 0xFFu,
                         ((p->twin_off & 0xFFu) |
                          (~p->twin_mask & 0xFFu)) & 0xFFu,
                         ((p->twin_off >> 8) |
                          (~(p->twin_mask >> 8) & 0xFFu)) & 0xFFu);
        } else {
            snprintf(tex, sizeof tex, "blend=%u", (p->tpage >> 5) & 3);
        }
        const char *kind = p->kind == PRIM_POLY ? (p->nverts == 4 ? "quad" : "tri")
                         : p->kind == PRIM_RECT ? "rect"
                         : p->kind == PRIM_LINE ? "line" : "LOAD";
        char verts[80];
        verts[0] = 0;
        if (p->kind == PRIM_POLY) {
            int n = 0;
            for (int k = 0; k < p->nverts && k < 4; k++)
                n += snprintf(verts + n, sizeof verts - (size_t)n, "%s%d,%d",
                              k ? " " : "", p->vx[k] + p->ofx, p->vy[k] + p->ofy);
        }
        fprintf(f, "%-5u %-5s %-24s %-11s %-9s %-52s ofs=%d,%d %s\n",
                i, kind, rect, col, flags, tex, p->ofx, p->ofy, verts);
    }
    if (prims_dropped)
        fprintf(f, "\n... %u further primitives not recorded (buffer full)\n",
                prims_dropped);
    fclose(f);
}

static void write_history(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/history.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# one row per display frame, oldest first.\n"
        "# stream/ring : the KCd request state and how many sectors are queued\n"
        "# gmv         : the movie player's own state bytes\n"
        "# mdec        : cumulative DecDCTin / DecDCTout calls\n"
        "# prims       : polys/rects/lines/LoadImages submitted that frame\n"
        "# clr         : screen clears that frame (ClearImage or a background\n"
        "#               DRAWENV). Neither goes through the ordering table, so\n"
        "#               a frame can clear the screen with prims all zero.\n\n");
    fprintf(f, "%-8s %-6s %-5s %-20s %-20s %-9s %-8s %-6s %-16s %s\n",
            "frame", "stream", "ring", "gmv b[0..5]", "scene g/s/c/r",
            "cd lba", "credit", "mdec", "prims", "clr");
    unsigned start = (hist_count < HIST_FRAMES) ? 0 : hist_head;
    for (unsigned i = 0; i < hist_count; i++) {
        const hist_row_t *h = &hist[(start + i) % HIST_FRAMES];
        char gmv[32], scene[32], mdec[24], prims_s[32];
        snprintf(gmv, sizeof gmv, "%u %u %u %u %u %u",
                 h->gmv[0], h->gmv[1], h->gmv[2], h->gmv[3], h->gmv[4], h->gmv[5]);
        snprintf(scene, sizeof scene, "%d/%d/%d/%d",
                 (int)h->scene[0], (int)h->scene[1],
                 (int)h->scene[2], (int)h->scene[3]);
        snprintf(mdec, sizeof mdec, "%u/%u", h->mdec_in, h->mdec_out);
        snprintf(prims_s, sizeof prims_s, "%u/%u/%u/%u",
                 h->polys, h->rects, h->lines, h->loads);
        fprintf(f, "%-8u %-6u %-5u %-20s %-20s %-9u %-8d %-6s %-16s %-4u%s\n",
                h->frame, h->stream_state, h->ring_fill, gmv, scene,
                h->cd_lba, h->cd_credit, mdec, prims_s, h->clears,
                h->cd_streaming ? "  [reading]" : "");
    }
    fclose(f);
}

static void write_report(const char *dir, ctx_t *ctx) {
    char path[512];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;

    ac3_cd_snap_t cd;
    ac3_mdec_snap_t md;
    ac3_cd_snapshot(&cd);
    ac3_mdec_snapshot(&md);

    fprintf(f, "Ace Combat 3 -- recompiled : frame capture %u\n", capture_seq);
    fprintf(f, "display frame  : %u\n\n", ac3_vblank_count());

    fprintf(f, "-- this frame's display list -------------------------------\n");
    fprintf(f, "polygons   %u\nrectangles %u\nlines      %u\nLoadImage  %u\n",
            frame_polys, frame_rects, frame_lines, frame_loads);
    fprintf(f, "(detail in drawlist.txt)\n\n");

    {
        fprintf(f, "-- palettes this frame ------------------------------------\n");
        struct { u16 clut; unsigned refs; } seen[64];
        unsigned n = 0, blank = 0, blank_refs = 0;
        for (unsigned i = 0; i < nprims; i++) {
            const prim_rec_t *p = &prims[i];
            if (p->kind != PRIM_POLY && p->kind != PRIM_RECT) continue;
            if (!(p->flags & 1)) continue;
            if (((p->tpage >> 7) & 3) == 2) continue;
            unsigned j = 0;
            for (; j < n; j++) if (seen[j].clut == p->clut) break;
            if (j == n) {
                if (n >= 64) continue;
                seen[n].clut = p->clut; seen[n].refs = 0; n++;
            }
            seen[j].refs++;
        }
        for (unsigned j = 0; j < n; j++) {
            int cx = (seen[j].clut & 0x3F) * 16, cy = (seen[j].clut >> 6) & 0x1FF;
            int nz = 0;
            if (ac3_vram && cx + 16 <= GPU_VRAM_W && cy < GPU_VRAM_H)
                for (int k = 0; k < 16; k++) if (*vram_at(cx + k, cy)) nz++;
            fprintf(f, "  (%4d,%3d)  %4u refs  %2d/16 entries set  ",
                    cx, cy, seen[j].refs, nz);
            u32 when = 0; const char *what = "?";
            int wx = 0, wy = 0, ww = 0, wh = 0; u8 wr = 0, wg = 0, wb = 0;
            if (ac3_vram_last_write_info(cx, cy, &when, &what, &wx, &wy,
                                         &ww, &wh, &wr, &wg, &wb)) {
                fprintf(f, "last written on frame %u by %s of (%d,%d) %dx%d",
                        when, what, wx, wy, ww, wh);
                if (wr || wg || wb) fprintf(f, " rgb(%u,%u,%u)", wr, wg, wb);
                else if (strcmp(what, "a fill") == 0)
                    fprintf(f, " rgb(0,0,0) BLACK");
                fprintf(f, "\n");
            } else {
                fprintf(f, "never written\n");
            }
            if (!nz) { blank++; blank_refs += seen[j].refs; }
        }
        if (!n) fprintf(f, "  (no paletted primitives this frame)\n");
        else if (blank)
            fprintf(f, "  VERDICT: %u of %u palettes are blank, drawing %u "
                       "primitives into nothing\n", blank, n, blank_refs);
        else
            fprintf(f, "  all %u palettes have content\n", n);
        fprintf(f, "\n");
    }

    fprintf(f, "-- video decoder (libpress / MDEC) -------------------------\n");
    fprintf(f, "DecDCTin calls this video : %u\n", md.frames);
    fprintf(f, "DecDCTin total / DecDCTout total : %u / %u\n",
            mdec_in_total, mdec_out_total);
    fprintf(f, "coefficient buffer : 0x%08X  cursor 0x%08X  end 0x%08X\n",
            md.coef_buf, md.coef_pos, md.coef_end);
    fprintf(f, "output depth       : %s\n", md.out_24bit ? "24bpp" : "15bpp");
    fprintf(f, "last frame decoded : %u blocks, %u of them empty (%u%%), "
               "%u coefficients\n",
            md.blocks, md.blocks_empty,
            md.blocks ? md.blocks_empty * 100 / md.blocks : 0, md.coefs);
    fprintf(f, "                     DC range %d .. %d, %u pixel bytes out\n",
            md.dc_min, md.dc_max, md.out_bytes);
    if (md.blocks && md.blocks_empty == md.blocks)
        fprintf(f, "  NOTE: every block was header+EOB with no coefficients --\n"
                   "        this frame was black before the IDCT ran.\n");
    fprintf(f, "\n");

    fprintf(f, "-- disc --------------------------------------------------\n");
    fprintf(f, "streaming %d   lba %u   mode 0x%02X   speed %d sectors/s\n",
            cd.streaming, cd.lba, cd.mode, cd.rate);
    fprintf(f, "sector credit %d   fifo lba %u valid %d\n",
            cd.credit, cd.fifo_lba, cd.fifo_valid);
    fprintf(f, "XA filter %s file %u chan %u\n\n",
            cd.filter_active ? "on" : "off", cd.filter_file, cd.filter_chan);

    if (ctx) {
        u32 gp = ctx->r[28];
        u32 g = rd32(gp + 748);
        u32 req = rd32(gp + 56), ring = rd32(gp + 64);
        u32 gc = rd32(gp + 1236);
        fprintf(f, "-- game state (via $gp = 0x%08X) --------------------------\n", gp);
        fprintf(f, "scene globals 0x%08X : group %d sub %d current %d requested %d\n",
                g, (int)rd32(g + 44), (int)rd32(g + 48),
                (int)rd32(g + 52), (int)rd32(g + 56));
        fprintf(f, "stream request 0x%08X state %u\n", req, rd32(req + 32));
        fprintf(f, "sector ring    0x%08X fill %u\n", ring, rd32(ring + 52));
        fprintf(f, "gmv player     0x%08X bytes", gc);
        if (gc) for (int i = 0; i < 16; i++) fprintf(f, " %02X", rd8(gc + (u32)i));
        fprintf(f, "\n\n");
    }

    {
        PSX_DISPENV d; PSX_DRAWENV w;
        ac3_gpu_env_snapshot(&d, &w);
        fprintf(f, "-- display ------------------------------------------------\n");
        fprintf(f, "DISPENV  vram (%d,%d) %dx%d   screen (%d,%d) %dx%d   "
                   "%s %s\n",
                d.disp.x, d.disp.y, d.disp.w, d.disp.h,
                d.screen.x, d.screen.y, d.screen.w, d.screen.h,
                d.isrgb24 ? "24bpp" : "15bpp", d.isinter ? "interlaced" : "");
        fprintf(f, "DRAWENV  clip (%d,%d) %dx%d   offset (%d,%d)   tpage 0x%03X\n",
                w.clip.x, w.clip.y, w.clip.w, w.clip.h,
                w.ofs[0], w.ofs[1], w.tpage);
        fprintf(f, "clears this frame %u   background DRAWENV %s   "
                   "last clear (%d,%d) %dx%d rgb(%u,%u,%u) via %s\n\n",
                frame_clears, w.isbg ? "yes" : "no",
                last_clear.x, last_clear.y, last_clear.w, last_clear.h,
                last_clear_rgb[0], last_clear_rgb[1], last_clear_rgb[2],
                last_clear_who);
    }

    fprintf(f, "-- recent VRAM traffic (uploads, copies and fills) ---------\n");
    fprintf(f, "total uploads this run: %u\n", load_total);
    {
        unsigned s = (load_count < LOAD_HIST) ? 0 : load_head;
        for (unsigned i = 0; i < load_count; i++) {
            const load_rec_t *lr = &load_hist[(s + i) % LOAD_HIST];
            if (lr->kind == VT_LOAD)
                fprintf(f, "[f%6u] vram (%4d,%4d) %4dx%-4d  <- guest 0x%08X\n",
                        lr->frame, lr->x, lr->y, lr->w, lr->h, lr->src);
            else if (lr->kind == VT_FILL)
                fprintf(f, "[f%6u] vram (%4d,%4d) %4dx%-4d  <- fill "
                           "rgb(%3u,%3u,%3u)%s via %s\n",
                        lr->frame, lr->x, lr->y, lr->w, lr->h,
                        lr->r, lr->g, lr->b,
                        (lr->r >> 3 || lr->g >> 3 || lr->b >> 3) ? "" : " BLACK",
                        lr->who ? lr->who : "?");
            else
                fprintf(f, "[f%6u] vram (%4d,%4d) %4dx%-4d  <- vram (%4d,%4d)"
                           "  copy%s\n",
                        lr->frame, lr->dx, lr->dy, lr->w, lr->h, lr->x, lr->y,
                        lr->kind == VT_COPY_SKIPPED
                            ? " SKIPPED: source never held" : "");
        }
    }
    fprintf(f, "\n");

    fprintf(f, "-- recent MDEC activity ----------------------------------\n");
    unsigned start = (mdec_count < MDEC_EVENTS) ? 0 : mdec_head;
    for (unsigned i = 0; i < mdec_count; i++) {
        const mdec_ev_t *e = &mdec_ev[(start + i) % MDEC_EVENTS];
        if (e->kind == 0)
            fprintf(f, "[f%6u] DecDCTin   buf=0x%08X cmd=0x%08X words=%u %s\n",
                    e->frame, e->addr, e->a, e->words, e->b ? "24bpp" : "15bpp");
        else
            fprintf(f, "[f%6u] DecDCTout  dst=0x%08X words=%u -> %u/%u mb, "
                       "%u stream bytes\n",
                    e->frame, e->addr, e->words, e->a, e->b, e->c);
    }
    fclose(f);
}

void ac3_capture_frame(ctx_t *ctx) {
    ac3_cd_snap_t cd;
    ac3_mdec_snap_t md;
    if (ctx) ac3_probe_queue(ctx);
    ac3_cd_snapshot(&cd);
    ac3_mdec_snapshot(&md);

    hist_row_t *h = &hist[hist_head];
    hist_head = (hist_head + 1) % HIST_FRAMES;
    if (hist_count < HIST_FRAMES) hist_count++;
    memset(h, 0, sizeof *h);
    h->frame = ac3_vblank_count();
    h->cd_lba = cd.lba;
    h->cd_credit = cd.credit;
    h->cd_streaming = (u8)cd.streaming;
    h->cd_mode = (u8)cd.mode;
    h->filt_file = cd.filter_file;
    h->filt_chan = cd.filter_chan;
    h->mdec_in = mdec_in_total;
    h->mdec_out = mdec_out_total;
    h->polys = (u16)(frame_polys > 0xFFFF ? 0xFFFF : frame_polys);
    h->rects = (u16)(frame_rects > 0xFFFF ? 0xFFFF : frame_rects);
    h->lines = (u16)(frame_lines > 0xFFFF ? 0xFFFF : frame_lines);
    h->loads = (u16)(frame_loads > 0xFFFF ? 0xFFFF : frame_loads);
    h->clears = (u16)(frame_clears > 0xFFFF ? 0xFFFF : frame_clears);
    if (ctx) {
        u32 gp = ctx->r[28];
        u32 g = rd32(gp + 748);
        u32 req = rd32(gp + 56), ring = rd32(gp + 64);
        u32 gc = rd32(gp + 1236);
        h->stream_state = rd32(req + 32);
        h->ring_fill = rd32(ring + 52);
        h->scene[0] = rd32(g + 44); h->scene[1] = rd32(g + 48);
        h->scene[2] = rd32(g + 52); h->scene[3] = rd32(g + 56);
        for (int i = 0; i < 8; i++) h->gmv[i] = (u8)rd8(gc + (u32)i);
    }

    if (pending && pending_needs_draw) {
        if (!frame_polys && !frame_rects && !frame_lines) {
            if (++pending_waited <= 8) {
                nprims = prims_dropped = 0;
                frame_polys = frame_rects = frame_lines = frame_loads = 0;
                frame_clears = 0;
                return;
            }
            printf("\n*** capture: no frame rasterised anything within %d frames, "
                   "taking this one anyway\n\n", pending_waited);
        }
        pending_needs_draw = 0;
    }

    if (pending) {
        pending = 0;
        char dir[512];
        dir_make("capture");
        snprintf(dir, sizeof dir, "capture/%03u", capture_seq);
        dir_make(dir);

        write_report(dir, ctx);
        write_prims(dir);
        write_history(dir);
        write_log(dir);
        write_vram(dir);
        write_ram(dir);
        write_mdec_pixels(dir);

        u32 len = 0;
        const u8 *stream = ac3_mdec_last_stream(&len);
        if (stream && len) {
            char path[512];
            snprintf(path, sizeof path, "%s/mdec_stream.bin", dir);
            FILE *sf = fopen(path, "wb");
            if (sf) { fwrite(stream, 1, len, sf); fclose(sf); }
        }

        char shot[512];
        snprintf(shot, sizeof shot, "%s/screen.png", dir);
        ac3_render_screenshot(shot);

        printf("\n*** capture %u written to %s "
               "(frame %u, %u primitives)\n\n",
               capture_seq, dir, ac3_vblank_count(), nprims);
        fflush(stdout);
        capture_seq++;
    }

    nprims = prims_dropped = 0;
    frame_polys = frame_rects = frame_lines = frame_loads = 0;
    frame_clears = 0;
}
