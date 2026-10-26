#include "ac3_runtime.h"
#include "pgxp.h"
#include <stdio.h>
#include <string.h>

static void hle_gte_quirk(const char *what) {
    static const char *seen[8];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 8) seen[n++] = what;
    fprintf(stderr, "ac3: GTE quirk exercised: %s\n", what);
}

#define MAC_MAX ((s64)0x7FFFFFFFFFFLL)
#define MAC_MIN (-(s64)0x80000000000LL)

#define FLAG_MAC1_POS (1u << 30)
#define FLAG_MAC2_POS (1u << 29)
#define FLAG_MAC3_POS (1u << 28)
#define FLAG_MAC1_NEG (1u << 27)
#define FLAG_MAC2_NEG (1u << 26)
#define FLAG_MAC3_NEG (1u << 25)
#define FLAG_IR1      (1u << 24)
#define FLAG_IR2      (1u << 23)
#define FLAG_IR3      (1u << 22)
#define FLAG_R_SAT    (1u << 21)
#define FLAG_G_SAT    (1u << 20)
#define FLAG_B_SAT    (1u << 19)
#define FLAG_SZ3      (1u << 18)
#define FLAG_DIV      (1u << 17)
#define FLAG_MAC0_POS (1u << 16)
#define FLAG_MAC0_NEG (1u << 15)
#define FLAG_SX2      (1u << 14)
#define FLAG_SY2      (1u << 13)
#define FLAG_IR0      (1u << 12)

#define FLAG_ERROR_MASK 0x7F87E000u

static void flag_finalize(gte_t *g) {
    if (g->flag & FLAG_ERROR_MASK) g->flag |= 0x80000000u;
}

static s64 mac_trunc(s64 v) {
    return (s64)((u64)v << 20) >> 20;
}

static s64 chk_mac(gte_t *g, int n, s64 v) {
    static const u32 pos[3] = { FLAG_MAC1_POS, FLAG_MAC2_POS, FLAG_MAC3_POS };
    static const u32 neg[3] = { FLAG_MAC1_NEG, FLAG_MAC2_NEG, FLAG_MAC3_NEG };
    if (v > MAC_MAX) g->flag |= pos[n - 1];
    else if (v < MAC_MIN) g->flag |= neg[n - 1];
    return mac_trunc(v);
}

static s32 chk_mac0(gte_t *g, s64 v) {
    if (v > (s64)2147483647LL) g->flag |= FLAG_MAC0_POS;
    else if (v < (s64)(-2147483647LL - 1)) g->flag |= FLAG_MAC0_NEG;
    return (s32)v;
}

static s16 chk_ir(gte_t *g, int n, s32 v, int lm) {
    static const u32 bit[3] = { FLAG_IR1, FLAG_IR2, FLAG_IR3 };
    s32 lo = lm ? 0 : -32768;
    if (v > 32767) { g->flag |= bit[n - 1]; return 32767; }
    if (v < lo)    { g->flag |= bit[n - 1]; return (s16)lo; }
    return (s16)v;
}

static s16 chk_ir0(gte_t *g, s32 v) {
    if (v > 0x1000) { g->flag |= FLAG_IR0; return 0x1000; }
    if (v < 0)      { g->flag |= FLAG_IR0; return 0; }
    return (s16)v;
}

static u16 chk_sz3(gte_t *g, s32 v) {
    if (v > 65535) { g->flag |= FLAG_SZ3; return 65535; }
    if (v < 0)     { g->flag |= FLAG_SZ3; return 0; }
    return (u16)v;
}

static s16 chk_sxy(gte_t *g, s32 v, int is_y) {
    if (v > 1023)  { g->flag |= is_y ? FLAG_SY2 : FLAG_SX2; return 1023; }
    if (v < -1024) { g->flag |= is_y ? FLAG_SY2 : FLAG_SX2; return -1024; }
    return (s16)v;
}

static u8 chk_col(gte_t *g, s32 v, int idx) {
    static const u32 bit[3] = { FLAG_R_SAT, FLAG_G_SAT, FLAG_B_SAT };
    if (v > 255) { g->flag |= bit[idx]; return 255; }
    if (v < 0)   { g->flag |= bit[idx]; return 0; }
    return (u8)v;
}

static void push_sz(gte_t *g, s32 v) {
    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];
    g->sz[3] = chk_sz3(g, v);
}

static void push_sxy(gte_t *g, s32 x, s32 y) {
    g->sxy[0][0] = g->sxy[1][0]; g->sxy[0][1] = g->sxy[1][1];
    g->sxy[1][0] = g->sxy[2][0]; g->sxy[1][1] = g->sxy[2][1];
    g->sxy[2][0] = chk_sxy(g, x, 0);
    g->sxy[2][1] = chk_sxy(g, y, 1);
}

static void push_rgb(gte_t *g, s32 r, s32 b_g, s32 b) {
    u8 rr = chk_col(g, r, 0), gg = chk_col(g, b_g, 1), bb = chk_col(g, b, 2);
    g->rgb[0] = g->rgb[1];
    g->rgb[1] = g->rgb[2];
    g->rgb[2] = ((g->rgbc >> 24) << 24) | ((u32)bb << 16) | ((u32)gg << 8) | rr;
}

static void push_rgb_from_mac(gte_t *g) {
    push_rgb(g, g->mac[1] >> 4, g->mac[2] >> 4, g->mac[3] >> 4);
}

static u8 unr_table[257];

static void unr_init(void) {
    for (int i = 0; i <= 256; i++) {
        s32 v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (u8)(v < 0 ? 0 : v);
    }
}

static u32 gte_divide(gte_t *g, u16 h, u16 sz3) {
    if (h >= sz3 * 2) {
        g->flag |= FLAG_DIV;
        return 0x1FFFF;
    }
    int z = 0;
    for (u32 t = sz3; t < 0x8000u && z < 16; t <<= 1) z++;
    u32 n = (u32)h << z;
    u32 d = (u32)sz3 << z;
    u32 u = unr_table[(d - 0x7FC0) >> 7] + 0x101u;
    d = (u32)(((0x2000080ULL - (u64)d * u) >> 8) & 0xFFFFFFFFu);
    d = (u32)(((0x0000080ULL + (u64)d * u) >> 8) & 0xFFFFFFFFu);
    u64 r = (((u64)n * d) + 0x8000ULL) >> 16;
    return (r > 0x1FFFF) ? 0x1FFFF : (u32)r;
}

static void set_mac_ir(gte_t *g, int n, s64 v, int shift, int lm) {
    s64 m = chk_mac(g, n, v);
    m >>= shift;
    g->mac[n] = (s32)m;
    g->ir[n] = chk_ir(g, n, (s32)m, lm);
}

static void mat_vec(gte_t *g, const s16 m[3][3], const s32 t[3],
                    const s16 v[3], int shift, int lm) {
    for (int i = 0; i < 3; i++) {
        s64 acc = (s64)t[i] * 0x1000;
        acc += (s64)m[i][0] * v[0];
        acc += (s64)m[i][1] * v[1];
        acc += (s64)m[i][2] * v[2];
        set_mac_ir(g, i + 1, acc, shift, lm);
    }
}

static void interpolate(gte_t *g, s64 m1, s64 m2, s64 m3, int shift, int lm) {
    set_mac_ir(g, 1, ((s64)g->fc[0] << 12) - m1, shift, 0);
    set_mac_ir(g, 2, ((s64)g->fc[1] << 12) - m2, shift, 0);
    set_mac_ir(g, 3, ((s64)g->fc[2] << 12) - m3, shift, 0);
    set_mac_ir(g, 1, (s64)g->ir[1] * g->ir[0] + m1, shift, lm);
    set_mac_ir(g, 2, (s64)g->ir[2] * g->ir[0] + m2, shift, lm);
    set_mac_ir(g, 3, (s64)g->ir[3] * g->ir[0] + m3, shift, lm);
}

static void vec_from_rgb(gte_t *g, s64 *r, s64 *gg, s64 *b) {
    *r  = (s64)(u8)(g->rgbc & 0xFF) * g->ir[1] * 16;
    *gg = (s64)(u8)((g->rgbc >> 8) & 0xFF) * g->ir[2] * 16;
    *b  = (s64)(u8)((g->rgbc >> 16) & 0xFF) * g->ir[3] * 16;
}

static void rtp(gte_t *g, int vi, int shift, int lm, int last) {
    s16 v[3] = { g->v[vi][0], g->v[vi][1], g->v[vi][2] };

    s64 z = 0;
    for (int i = 0; i < 3; i++) {
        s64 acc = (s64)g->tr[i] * 0x1000;
        acc += (s64)g->rt[i][0] * v[0];
        acc += (s64)g->rt[i][1] * v[1];
        acc += (s64)g->rt[i][2] * v[2];
        if (i == 2) z = chk_mac(g, 3, acc);
        set_mac_ir(g, i + 1, acc, shift, lm);
    }
    push_sz(g, (s32)(z >> 12));

    u32 q = gte_divide(g, g->h, g->sz[3]);

    s64 px = (s64)q * g->ir[1];
#ifdef AC3_ENHANCED
    if (ac3_ws_factor != 1.0f) px = (s64)((double)px * ac3_ws_factor);
#endif
    s64 sx = px + g->ofx;
    s64 sy = (s64)q * g->ir[2] + g->ofy;
    chk_mac0(g, sx);
    chk_mac0(g, sy);
    push_sxy(g, (s32)(sx >> 16), (s32)(sy >> 16));

#ifdef AC3_ENHANCED
    if (!(g->flag & FLAG_DIV) && z > 0) {
        double vx = 0, vy = 0;
        for (int i = 0; i < 2; i++) {
            s64 acc = (s64)g->tr[i] * 0x1000;
            acc += (s64)g->rt[i][0] * v[0];
            acc += (s64)g->rt[i][1] * v[1];
            acc += (s64)g->rt[i][2] * v[2];
            if (i == 0) vx = (double)acc / 4096.0; else vy = (double)acc / 4096.0;
        }
        double vz = (double)z / 4096.0;
        double k = (double)g->h / vz;
        ac3_pgxp_vtx_t pv;
        pv.sx = (float)((double)g->ofx / 65536.0 + vx * k * ac3_ws_factor);
        pv.sy = (float)((double)g->ofy / 65536.0 + vy * k);
        pv.vx = (float)vx;
        pv.vy = (float)vy;
        pv.vz = (float)vz;
        u32 key = ((u32)(u16)g->sxy[2][1] << 16) | (u16)g->sxy[2][0];
        ac3_pgxp_record(key, &pv);
    }
#endif

    if (last) {
        s64 dp = (s64)q * g->dqa + g->dqb;
        g->mac[0] = chk_mac0(g, dp);
        g->ir[0] = chk_ir0(g, (s32)(dp >> 12));
#ifdef AC3_ENHANCED
        if (ac3_gte_no_depthcue) g->ir[0] = 0;
#endif
    }
}

void ac3_gte_op(ctx_t *ctx, u32 opcode) {
    gte_t *g = &ctx->gte;
    static int table_ready;
    if (!table_ready) { unr_init(); table_ready = 1; }

    u32 fn = opcode & 0x3F;
    int sf = (opcode >> 19) & 1;
    int shift = sf ? 12 : 0;
    int lm = (opcode >> 10) & 1;

    g->flag = 0;

    switch (fn) {
    case 0x01:
        rtp(g, 0, shift, lm, 1);
        break;

    case 0x30:
        rtp(g, 0, shift, lm, 0);
        rtp(g, 1, shift, lm, 0);
        rtp(g, 2, shift, lm, 1);
        break;

    case 0x06: {
        s64 v = (s64)g->sxy[0][0] * (g->sxy[1][1] - g->sxy[2][1])
              + (s64)g->sxy[1][0] * (g->sxy[2][1] - g->sxy[0][1])
              + (s64)g->sxy[2][0] * (g->sxy[0][1] - g->sxy[1][1]);
        g->mac[0] = chk_mac0(g, v);
        break;
    }

    case 0x0C: {
        s64 d1 = (s64)g->rt[0][0], d2 = (s64)g->rt[1][1], d3 = (s64)g->rt[2][2];
        s64 m1 = d2 * g->ir[3] - d3 * g->ir[2];
        s64 m2 = d3 * g->ir[1] - d1 * g->ir[3];
        s64 m3 = d1 * g->ir[2] - d2 * g->ir[1];
        set_mac_ir(g, 1, m1, shift, lm);
        set_mac_ir(g, 2, m2, shift, lm);
        set_mac_ir(g, 3, m3, shift, lm);
        break;
    }

    case 0x10: {
        s64 r = (s64)(u8)(g->rgbc & 0xFF) << 16;
        s64 gg = (s64)(u8)((g->rgbc >> 8) & 0xFF) << 16;
        s64 b = (s64)(u8)((g->rgbc >> 16) & 0xFF) << 16;
        interpolate(g, r, gg, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x2A: {
        for (int i = 0; i < 3; i++) {
            s64 r = (s64)(u8)(g->rgb[0] & 0xFF) << 16;
            s64 gg = (s64)(u8)((g->rgb[0] >> 8) & 0xFF) << 16;
            s64 b = (s64)(u8)((g->rgb[0] >> 16) & 0xFF) << 16;
            interpolate(g, r, gg, b, shift, lm);
            push_rgb_from_mac(g);
        }
        break;
    }

    case 0x11: {
        s64 m1 = (s64)g->ir[1] << 12;
        s64 m2 = (s64)g->ir[2] << 12;
        s64 m3 = (s64)g->ir[3] << 12;
        interpolate(g, m1, m2, m3, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x12: {
        u32 mx = (opcode >> 17) & 3;
        u32 vx = (opcode >> 15) & 3;
        u32 cv = (opcode >> 13) & 3;

        s16 vec[3];
        if (vx < 3) {
            vec[0] = g->v[vx][0]; vec[1] = g->v[vx][1]; vec[2] = g->v[vx][2];
        } else {
            vec[0] = g->ir[1]; vec[1] = g->ir[2]; vec[2] = g->ir[3];
        }

        s16 mtx[3][3];
        if (mx == 0)      memcpy(mtx, g->rt, sizeof mtx);
        else if (mx == 1) memcpy(mtx, g->llm, sizeof mtx);
        else if (mx == 2) memcpy(mtx, g->lcm, sizeof mtx);
        else {
            s16 r = (s16)((g->rgbc & 0xFF) << 4);
            mtx[0][0] = -r;      mtx[0][1] = r;        mtx[0][2] = g->ir[0];
            mtx[1][0] = g->rt[0][2]; mtx[1][1] = g->rt[0][2]; mtx[1][2] = g->rt[0][2];
            mtx[2][0] = g->rt[1][1]; mtx[2][1] = g->rt[1][1]; mtx[2][2] = g->rt[1][1];
            hle_gte_quirk("MVMVA garbage matrix");
        }

        const s32 zero[3] = { 0, 0, 0 };
        const s32 *tv = zero;
        if (cv == 0) tv = g->tr;
        else if (cv == 1) tv = g->bk;
        else if (cv == 2) {
            for (int i = 0; i < 3; i++) {
                s64 acc = (s64)g->fc[i] * 0x1000;
                acc += (s64)mtx[i][0] * vec[0];
                chk_mac(g, i + 1, acc);
                s64 rest = (s64)mtx[i][1] * vec[1] + (s64)mtx[i][2] * vec[2];
                chk_ir(g, i + 1, (s32)(mac_trunc(acc) >> shift), 0);
                set_mac_ir(g, i + 1, rest, shift, lm);
            }
            hle_gte_quirk("MVMVA cv=FC");
            break;
        }
        mat_vec(g, (const s16 (*)[3])mtx, tv, vec, shift, lm);
        break;
    }

    case 0x13: {
        mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                (const s16[3]){ g->v[0][0], g->v[0][1], g->v[0][2] }, shift, lm);
        mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
        s64 r, gg, b;
        vec_from_rgb(g, &r, &gg, &b);
        interpolate(g, r, gg, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x16: {
        for (int i = 0; i < 3; i++) {
            mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                    (const s16[3]){ g->v[i][0], g->v[i][1], g->v[i][2] }, shift, lm);
            mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                    (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
            s64 r, gg, b;
            vec_from_rgb(g, &r, &gg, &b);
            interpolate(g, r, gg, b, shift, lm);
            push_rgb_from_mac(g);
        }
        break;
    }

    case 0x1B: {
        mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                (const s16[3]){ g->v[0][0], g->v[0][1], g->v[0][2] }, shift, lm);
        mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
        s64 r, gg, b;
        vec_from_rgb(g, &r, &gg, &b);
        set_mac_ir(g, 1, r, shift, lm);
        set_mac_ir(g, 2, gg, shift, lm);
        set_mac_ir(g, 3, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x3F: {
        for (int i = 0; i < 3; i++) {
            mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                    (const s16[3]){ g->v[i][0], g->v[i][1], g->v[i][2] }, shift, lm);
            mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                    (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
            s64 r, gg, b;
            vec_from_rgb(g, &r, &gg, &b);
            set_mac_ir(g, 1, r, shift, lm);
            set_mac_ir(g, 2, gg, shift, lm);
            set_mac_ir(g, 3, b, shift, lm);
            push_rgb_from_mac(g);
        }
        break;
    }

    case 0x1E: {
        mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                (const s16[3]){ g->v[0][0], g->v[0][1], g->v[0][2] }, shift, lm);
        mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x20: {
        for (int i = 0; i < 3; i++) {
            mat_vec(g, (const s16 (*)[3])g->llm, (const s32[3]){0,0,0},
                    (const s16[3]){ g->v[i][0], g->v[i][1], g->v[i][2] }, shift, lm);
            mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                    (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
            push_rgb_from_mac(g);
        }
        break;
    }

    case 0x14: {
        mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
        s64 r, gg, b;
        vec_from_rgb(g, &r, &gg, &b);
        interpolate(g, r, gg, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x1C: {
        mat_vec(g, (const s16 (*)[3])g->lcm, g->bk,
                (const s16[3]){ g->ir[1], g->ir[2], g->ir[3] }, shift, lm);
        s64 r, gg, b;
        vec_from_rgb(g, &r, &gg, &b);
        set_mac_ir(g, 1, r, shift, lm);
        set_mac_ir(g, 2, gg, shift, lm);
        set_mac_ir(g, 3, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x28:
        set_mac_ir(g, 1, (s64)g->ir[1] * g->ir[1], shift, lm);
        set_mac_ir(g, 2, (s64)g->ir[2] * g->ir[2], shift, lm);
        set_mac_ir(g, 3, (s64)g->ir[3] * g->ir[3], shift, lm);
        break;

    case 0x29: {
        s64 r, gg, b;
        vec_from_rgb(g, &r, &gg, &b);
        interpolate(g, r, gg, b, shift, lm);
        push_rgb_from_mac(g);
        break;
    }

    case 0x2D: {
        s64 v = (s64)g->zsf3 * ((s32)g->sz[1] + g->sz[2] + g->sz[3]);
        g->mac[0] = chk_mac0(g, v);
        g->otz = chk_sz3(g, (s32)(v >> 12));
        break;
    }

    case 0x2E: {
        s64 v = (s64)g->zsf4 * ((s32)g->sz[0] + g->sz[1] + g->sz[2] + g->sz[3]);
        g->mac[0] = chk_mac0(g, v);
        g->otz = chk_sz3(g, (s32)(v >> 12));
        break;
    }

    case 0x3D:
        set_mac_ir(g, 1, (s64)g->ir[0] * g->ir[1], shift, lm);
        set_mac_ir(g, 2, (s64)g->ir[0] * g->ir[2], shift, lm);
        set_mac_ir(g, 3, (s64)g->ir[0] * g->ir[3], shift, lm);
        push_rgb_from_mac(g);
        break;

    case 0x3E:
        set_mac_ir(g, 1, ((s64)g->mac[1] << shift) + (s64)g->ir[0] * g->ir[1],
                   shift, lm);
        set_mac_ir(g, 2, ((s64)g->mac[2] << shift) + (s64)g->ir[0] * g->ir[2],
                   shift, lm);
        set_mac_ir(g, 3, ((s64)g->mac[3] << shift) + (s64)g->ir[0] * g->ir[3],
                   shift, lm);
        push_rgb_from_mac(g);
        break;

    default:
        fprintf(stderr, "ac3: unimplemented GTE command 0x%02X\n", fn);
        break;
    }

    flag_finalize(g);
}

static u32 pack_xy(s16 x, s16 y) {
    return ((u32)(u16)y << 16) | (u32)(u16)x;
}

u32 ac3_gte_mfc2(ctx_t *ctx, u32 reg) {
    gte_t *g = &ctx->gte;
    switch (reg) {
    case 0:  return pack_xy(g->v[0][0], g->v[0][1]);
    case 1:  return (u32)(s32)g->v[0][2];
    case 2:  return pack_xy(g->v[1][0], g->v[1][1]);
    case 3:  return (u32)(s32)g->v[1][2];
    case 4:  return pack_xy(g->v[2][0], g->v[2][1]);
    case 5:  return (u32)(s32)g->v[2][2];
    case 6:  return g->rgbc;
    case 7:  return g->otz;
    case 8:  return (u32)(s32)g->ir[0];
    case 9:  return (u32)(s32)g->ir[1];
    case 10: return (u32)(s32)g->ir[2];
    case 11: return (u32)(s32)g->ir[3];
    case 12: return pack_xy(g->sxy[0][0], g->sxy[0][1]);
    case 13: return pack_xy(g->sxy[1][0], g->sxy[1][1]);
    case 14:
    case 15: return pack_xy(g->sxy[2][0], g->sxy[2][1]);
    case 16: return g->sz[0];
    case 17: return g->sz[1];
    case 18: return g->sz[2];
    case 19: return g->sz[3];
    case 20: return g->rgb[0];
    case 21: return g->rgb[1];
    case 22: return g->rgb[2];
    case 23: return g->res1;
    case 24: return (u32)g->mac[0];
    case 25: return (u32)g->mac[1];
    case 26: return (u32)g->mac[2];
    case 27: return (u32)g->mac[3];
    case 28:
    case 29: {
        s32 r = g->ir[1] / 0x80, gg = g->ir[2] / 0x80, b = g->ir[3] / 0x80;
        r = r < 0 ? 0 : (r > 0x1F ? 0x1F : r);
        gg = gg < 0 ? 0 : (gg > 0x1F ? 0x1F : gg);
        b = b < 0 ? 0 : (b > 0x1F ? 0x1F : b);
        return (u32)((b << 10) | (gg << 5) | r);
    }
    case 30: return (u32)g->lzcs;
    case 31: return (u32)g->lzcr;
    default: return 0;
    }
}

void ac3_gte_mtc2(ctx_t *ctx, u32 reg, u32 v) {
    gte_t *g = &ctx->gte;
    switch (reg) {
    case 0:  g->v[0][0] = (s16)v; g->v[0][1] = (s16)(v >> 16); break;
    case 1:  g->v[0][2] = (s16)v; break;
    case 2:  g->v[1][0] = (s16)v; g->v[1][1] = (s16)(v >> 16); break;
    case 3:  g->v[1][2] = (s16)v; break;
    case 4:  g->v[2][0] = (s16)v; g->v[2][1] = (s16)(v >> 16); break;
    case 5:  g->v[2][2] = (s16)v; break;
    case 6:  g->rgbc = v; break;
    case 7:  g->otz = (u16)v; break;
    case 8:  g->ir[0] = (s16)v; break;
    case 9:  g->ir[1] = (s16)v; break;
    case 10: g->ir[2] = (s16)v; break;
    case 11: g->ir[3] = (s16)v; break;
    case 12: g->sxy[0][0] = (s16)v; g->sxy[0][1] = (s16)(v >> 16); break;
    case 13: g->sxy[1][0] = (s16)v; g->sxy[1][1] = (s16)(v >> 16); break;
    case 14: g->sxy[2][0] = (s16)v; g->sxy[2][1] = (s16)(v >> 16); break;
    case 15:
        g->sxy[0][0] = g->sxy[1][0]; g->sxy[0][1] = g->sxy[1][1];
        g->sxy[1][0] = g->sxy[2][0]; g->sxy[1][1] = g->sxy[2][1];
        g->sxy[2][0] = (s16)v; g->sxy[2][1] = (s16)(v >> 16);
        break;
    case 16: g->sz[0] = (u16)v; break;
    case 17: g->sz[1] = (u16)v; break;
    case 18: g->sz[2] = (u16)v; break;
    case 19: g->sz[3] = (u16)v; break;
    case 20: g->rgb[0] = v; break;
    case 21: g->rgb[1] = v; break;
    case 22: g->rgb[2] = v; break;
    case 23: g->res1 = v; break;
    case 24: g->mac[0] = (s32)v; break;
    case 25: g->mac[1] = (s32)v; break;
    case 26: g->mac[2] = (s32)v; break;
    case 27: g->mac[3] = (s32)v; break;
    case 28:
        g->irgb = v & 0x7FFF;
        g->ir[1] = (s16)((v & 0x1F) * 0x80);
        g->ir[2] = (s16)(((v >> 5) & 0x1F) * 0x80);
        g->ir[3] = (s16)(((v >> 10) & 0x1F) * 0x80);
        break;
    case 29: break;
    case 30: {
        g->lzcs = (s32)v;
        u32 x = (v & 0x80000000u) ? ~v : v;
        int n = 0;
        while (n < 32 && !(x & 0x80000000u)) { n++; x <<= 1; }
        g->lzcr = n;
        break;
    }
    case 31: break;
    default: break;
    }
}

extern u32 ac3_guest_ra;   /* ac3_interp.c: the caller's return address when interpreted */
u32 ac3_gte_cfc2(ctx_t *ctx, u32 reg) {
    gte_t *g = &ctx->gte;
    switch (reg) {
    case 0:  return pack_xy(g->rt[0][0], g->rt[0][1]);
    case 1:  return pack_xy(g->rt[0][2], g->rt[1][0]);
    case 2:  return pack_xy(g->rt[1][1], g->rt[1][2]);
    case 3:  return pack_xy(g->rt[2][0], g->rt[2][1]);
    case 4:  return (u32)(s32)g->rt[2][2];
    case 5:  return (u32)g->tr[0];
    case 6:  return (u32)g->tr[1];
    case 7:  return (u32)g->tr[2];
    case 8:  return pack_xy(g->llm[0][0], g->llm[0][1]);
    case 9:  return pack_xy(g->llm[0][2], g->llm[1][0]);
    case 10: return pack_xy(g->llm[1][1], g->llm[1][2]);
    case 11: return pack_xy(g->llm[2][0], g->llm[2][1]);
    case 12: return (u32)(s32)g->llm[2][2];
    case 13: return (u32)g->bk[0];
    case 14: return (u32)g->bk[1];
    case 15: return (u32)g->bk[2];
    case 16: return pack_xy(g->lcm[0][0], g->lcm[0][1]);
    case 17: return pack_xy(g->lcm[0][2], g->lcm[1][0]);
    case 18: return pack_xy(g->lcm[1][1], g->lcm[1][2]);
    case 19: return pack_xy(g->lcm[2][0], g->lcm[2][1]);
    case 20: return (u32)(s32)g->lcm[2][2];
    case 21: return (u32)g->fc[0];
    case 22: return (u32)g->fc[1];
    case 23: return (u32)g->fc[2];
    case 24: return (u32)g->ofx;
    case 25: return (u32)g->ofy;
    case 26:
#ifdef AC3_ENHANCED
        if (ac3_ws_factor != 1.0f &&
            (ctx->r[31] == AC3_WS_FRUSTUM_CALLER || ac3_guest_ra == AC3_WS_FRUSTUM_CALLER))
            return (u32)(s32)(s16)(s32)((float)(s16)g->h * ac3_ws_factor);
#endif
        return (u32)(s32)(s16)g->h;
    case 27: return (u32)(s32)g->dqa;
    case 28: return (u32)g->dqb;
    case 29: return (u32)(s32)g->zsf3;
    case 30: return (u32)(s32)g->zsf4;
    case 31: return g->flag;
    default: return 0;
    }
}

void ac3_gte_ctc2(ctx_t *ctx, u32 reg, u32 v) {
    gte_t *g = &ctx->gte;
    switch (reg) {
    case 0:  g->rt[0][0] = (s16)v; g->rt[0][1] = (s16)(v >> 16); break;
    case 1:  g->rt[0][2] = (s16)v; g->rt[1][0] = (s16)(v >> 16); break;
    case 2:  g->rt[1][1] = (s16)v; g->rt[1][2] = (s16)(v >> 16); break;
    case 3:  g->rt[2][0] = (s16)v; g->rt[2][1] = (s16)(v >> 16); break;
    case 4:  g->rt[2][2] = (s16)v; break;
    case 5:  g->tr[0] = (s32)v; break;
    case 6:  g->tr[1] = (s32)v; break;
    case 7:  g->tr[2] = (s32)v; break;
    case 8:  g->llm[0][0] = (s16)v; g->llm[0][1] = (s16)(v >> 16); break;
    case 9:  g->llm[0][2] = (s16)v; g->llm[1][0] = (s16)(v >> 16); break;
    case 10: g->llm[1][1] = (s16)v; g->llm[1][2] = (s16)(v >> 16); break;
    case 11: g->llm[2][0] = (s16)v; g->llm[2][1] = (s16)(v >> 16); break;
    case 12: g->llm[2][2] = (s16)v; break;
    case 13: g->bk[0] = (s32)v; break;
    case 14: g->bk[1] = (s32)v; break;
    case 15: g->bk[2] = (s32)v; break;
    case 16: g->lcm[0][0] = (s16)v; g->lcm[0][1] = (s16)(v >> 16); break;
    case 17: g->lcm[0][2] = (s16)v; g->lcm[1][0] = (s16)(v >> 16); break;
    case 18: g->lcm[1][1] = (s16)v; g->lcm[1][2] = (s16)(v >> 16); break;
    case 19: g->lcm[2][0] = (s16)v; g->lcm[2][1] = (s16)(v >> 16); break;
    case 20: g->lcm[2][2] = (s16)v; break;
    case 21: g->fc[0] = (s32)v; break;
    case 22: g->fc[1] = (s32)v; break;
    case 23: g->fc[2] = (s32)v; break;
    case 24: g->ofx = (s32)v; break;
    case 25: g->ofy = (s32)v; break;
    case 26: g->h = (u16)v; break;
    case 27: g->dqa = (s16)v; break;
    case 28: g->dqb = (s32)v; break;
    case 29: g->zsf3 = (s16)v; break;
    case 30: g->zsf4 = (s16)v; break;
    case 31: g->flag = v & 0x7FFFF000u; flag_finalize(g); break;
    default: break;
    }
}
