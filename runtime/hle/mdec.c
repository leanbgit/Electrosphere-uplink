#include "hle.h"
#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const u8 ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

static u8 quant_table[64] = {
     2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

static double idct_cos[8][8];
static int idct_ready;

static void idct_init(void) {
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            idct_cos[x][u] = cos((2.0 * x + 1.0) * u * 3.14159265358979 / 16.0)
                             * (u == 0 ? 0.353553390593274 : 0.5);
    idct_ready = 1;
}

static void idct_8x8(const s16 *in, s16 *out) {
    double tmp[64];
    if (!idct_ready) idct_init();
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += idct_cos[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++) s += idct_cos[y][v] * tmp[v * 8 + x];
            int r = (int)(s + (s < 0 ? -0.5 : 0.5));
            out[y * 8 + x] = (s16)(r < -256 ? -256 : (r > 255 ? 255 : r));
        }
}

#define MDEC_EOB 0xFE00u

static u32 stat_blocks, stat_blocks_empty, stat_coefs;
static s32 stat_dc_min, stat_dc_max;

static void stats_reset(void) {
    stat_blocks = stat_blocks_empty = stat_coefs = 0;
    stat_dc_min = stat_dc_max = 0;
}

static int block_from_stream(u32 *addr, u32 end, s16 *out) {
    s16 coef[64];
    memset(coef, 0, sizeof coef);
    if (*addr + 2 > end) return 0;

    u32 head = MEM_R16U(*addr); *addr += 2;
    int q_scale = (int)((head >> 10) & 0x3F);
    int dc = (int)(head & 0x3FF);
    if (dc & 0x200) dc -= 0x400;
    coef[0] = (s16)(dc * quant_table[0]);

    if (!stat_blocks || dc < stat_dc_min) stat_dc_min = dc;
    if (!stat_blocks || dc > stat_dc_max) stat_dc_max = dc;
    stat_blocks++;
    u32 got = 0;

    int idx = 0;
    while (*addr + 2 <= end) {
        u32 w = MEM_R16U(*addr); *addr += 2;
        if (w == MDEC_EOB) break;
        int run = (int)((w >> 10) & 0x3F);
        int level = (int)(w & 0x3FF);
        if (level & 0x200) level -= 0x400;
        idx += run + 1;
        if (idx > 63) break;
        coef[ZIGZAG[idx]] =
            (s16)(level * quant_table[ZIGZAG[idx]] * q_scale / 8);
        got++;
    }
    stat_coefs += got;
    if (!got && dc == 0) stat_blocks_empty++;
    idct_8x8(coef, out);
    return 1;
}

static void yuv_to_rgb(int y, int cb, int cr, u8 *r, u8 *g, u8 *b) {
    int rr = y + ((91881 * cr) >> 16);
    int gg = y - ((22554 * cb + 46802 * cr) >> 16);
    int bb = y + ((116130 * cb) >> 16);
    rr += 128; gg += 128; bb += 128;
    *r = (u8)(rr < 0 ? 0 : (rr > 255 ? 255 : rr));
    *g = (u8)(gg < 0 ? 0 : (gg > 255 ? 255 : gg));
    *b = (u8)(bb < 0 ? 0 : (bb > 255 ? 255 : bb));
}

const char *ac3_mdec_dump;

static u32 coef_buf;
static u32 coef_pos;
static u32 coef_end;
static int out_24bit;
static int out_mask15;

#define MAX_COEF_BYTES (512 * 1024)
static int decode_failed;

#define CAP_STREAM_MAX (256 * 1024)
#define CAP_PIXEL_MAX  (512 * 1024)
static u8  cap_stream[CAP_STREAM_MAX];
static u32 cap_stream_len;
static u8  cap_pixels[CAP_PIXEL_MAX];
static u32 cap_pixels_len;
static u32 cap_strip_mb;

const u8 *ac3_mdec_last_stream(u32 *len) {
    if (len) *len = cap_stream_len;
    return cap_stream;
}

const u8 *ac3_mdec_last_pixels(u32 *len, int *bpp24, u32 *strip) {
    if (len) *len = cap_pixels_len;
    if (bpp24) *bpp24 = out_24bit;
    if (strip) *strip = cap_strip_mb;
    return cap_pixels;
}

void psyq_DecDCTReset(ctx_t *ctx) {
    coef_buf = 0;
    coef_pos = coef_end = 0;
    decode_failed = 0;
    RET(0);
}

static unsigned vid_frames, vid_start_vb;
extern u32 ac3_vblank_count(void);
extern void ac3_cd_stream_ahead(ctx_t *ctx);

void psyq_DecDCTin(ctx_t *ctx) {
    if (!A0) { RET(0); return; }

    if (!vid_frames) vid_start_vb = ac3_vblank_count();
    vid_frames++;
    if (vid_frames % 100 == 0) {
        unsigned vb = ac3_vblank_count() - vid_start_vb;
        hle_trace("mdec", "video: %u frames in %u vblanks = %.1f fps",
                  vid_frames, vb, vb ? 60.0 * vid_frames / vb : 0.0);
    }

    u32 cmd = MEM_R32(A0);
    u32 words = cmd & 0xFFFFu;
    if (words > MAX_COEF_BYTES / 4) words = MAX_COEF_BYTES / 4;

    cmd = (A1 & 1u) ? (cmd & ~0x08000000u) : (cmd | 0x08000000u);
    cmd = (A1 & 2u) ? (cmd | 0x02000000u) : (cmd & ~0x02000000u);
    MEM_W32(A0, cmd);

    coef_buf = A0;
    coef_pos = A0 + 4;
    coef_end = A0 + 4 + words * 4;
    out_24bit = ((cmd >> 27) & 3u) == 2u;
    out_mask15 = ((cmd >> 25) & 1u) != 0u;

    stats_reset();
    cap_pixels_len = 0;
    cap_strip_mb = 0;
    cap_stream_len = (coef_end - coef_pos) > CAP_STREAM_MAX
                     ? CAP_STREAM_MAX : (coef_end - coef_pos);
    for (u32 i = 0; i < cap_stream_len; i++) cap_stream[i] = (u8)MEM_R8U(coef_pos + i);
    ac3_capture_mdec_in(A0, cmd, words, out_24bit);
    ac3_probe_mdec_in(words, out_24bit);

    if (ac3_mdec_dump) {
        static unsigned seq;
        char path[256];
        snprintf(path, sizeof path, "%s/coef_%03u.bin", ac3_mdec_dump, seq++);
        FILE *f = fopen(path, "wb");
        if (f) {
            for (u32 i = coef_pos; i < coef_end; i++) fputc(MEM_R8U(i), f);
            fclose(f);
        }
    }

    ac3_cd_stream_ahead(ctx);
    RET(0);
}

void psyq_DecDCTinSync(ctx_t *ctx)  { RET(0); }
void psyq_DecDCToutSync(ctx_t *ctx) { RET(0); }
static u32 in_callback, out_callback;

void psyq_DecDCTinCallback(ctx_t *ctx) {
    u32 prev = in_callback;
    in_callback = A0;
    RET(prev);
}

void psyq_DecDCToutCallback(ctx_t *ctx) {
    if (!A0 && vid_frames) {
        unsigned vb = ac3_vblank_count() - vid_start_vb;
        hle_trace("mdec", "video ended: %u frames in %u vblanks = %.1f fps",
                  vid_frames, vb, vb ? 60.0 * vid_frames / vb : 0.0);
        vid_frames = 0;
    }
    hle_trace("DecDCToutCallback", "handler=0x%08X", A0);
    u32 prev = out_callback;
    out_callback = A0;
    RET(prev);
}

void psyq_DecDCTBufSize(ctx_t *ctx) { RET(A0); }

void psyq_DecDCTGetEnv(ctx_t *ctx) { RET(A0); }

void psyq_DecDCTPutEnv(ctx_t *ctx) {
    if (A0) {
        for (int i = 0; i < 64; i++)
            quant_table[i] = (u8)MEM_R8U(A0 + (u32)i);
    }
    RET(0);
}

void psyq_DecDCTout(ctx_t *ctx) {
    u32 dst = A0, words = A1;
    if (!coef_buf || !dst || !words) { RET(0); return; }

    u32 bytes = words * 4;
    u32 pos = coef_pos;
    u32 end = coef_end;

    u32 mb_bytes = 16 * 16 * (out_24bit ? 3 : 2);
    u32 mb_count = bytes / mb_bytes;
    u32 coef_pos_start = pos;
    s16 blk[6][64];
    u32 done = 0;
    for (u32 mb = 0; mb < mb_count; mb++) {
        for (int i = 0; i < 6; i++)
            if (!block_from_stream(&pos, end, blk[i])) {
                coef_pos = pos;
                ac3_capture_mdec_out(dst, words, done, mb_count,
                                     pos - coef_pos_start);
                RET(done);
                return;
            }

        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int lb = (y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0);
                int Y  = blk[2 + lb][(y & 7) * 8 + (x & 7)];
                int cb = blk[1][(y >> 1) * 8 + (x >> 1)];
                int cr = blk[0][(y >> 1) * 8 + (x >> 1)];
                u8 r, g, b;
                yuv_to_rgb(Y, cb, cr, &r, &g, &b);
                u32 px = mb * 256 + y * 16 + x;
                if (out_24bit) {
                    MEM_W8(dst + px * 3 + 0, r);
                    MEM_W8(dst + px * 3 + 1, g);
                    MEM_W8(dst + px * 3 + 2, b);
                } else {
                    MEM_W16(dst + px * 2,
                            (u16)((out_mask15 ? 0x8000u : 0u) |
                                  ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)));
                }
            }
        }
        done++;
    }

    coef_pos = pos;

    {
        u32 n = done * mb_bytes;
        if (n > CAP_PIXEL_MAX - cap_pixels_len) n = CAP_PIXEL_MAX - cap_pixels_len;
        for (u32 i = 0; i < n; i++)
            cap_pixels[cap_pixels_len + i] = (u8)MEM_R8U(dst + i);
        cap_pixels_len += n;
        if (!cap_strip_mb) cap_strip_mb = done;
    }
    ac3_capture_mdec_out(dst, words, done, mb_count, pos - coef_pos_start);

    hle_trace("DecDCTout", "dst=0x%08X words=%u %s -> %u/%u mb, %u bytes in",
              dst, words, out_24bit ? "24bpp" : "16bpp", done, mb_count,
              pos - coef_pos_start);
    if (out_callback) {
        u32 sa0 = ctx->r[4];
        ctx->r[4] = 1;
        ac3_call_indirect(ctx, out_callback);
        ctx->r[4] = sa0;
    }
    RET(done);
}

void ac3_mdec_snapshot(ac3_mdec_snap_t *s) {
    if (!s) return;
    s->coef_buf = coef_buf;
    s->coef_pos = coef_pos;
    s->coef_end = coef_end;
    s->out_24bit = out_24bit;
    s->frames = vid_frames;
    s->blocks = stat_blocks;
    s->blocks_empty = stat_blocks_empty;
    s->coefs = stat_coefs;
    s->dc_min = stat_dc_min;
    s->dc_max = stat_dc_max;
    s->out_bytes = cap_pixels_len;
}
