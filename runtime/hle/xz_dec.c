#include "xz_dec.h"
#include <stdlib.h>
#include <string.h>

#define LZ_BITS   11
#define LZ_TOTAL  (1u << LZ_BITS)
#define LZ_MOVE   5
#define LZ_STATES 12
#define LZ_POS    16
#define LZ_LIT    (0x300u * 16u)

typedef struct {
    const u8 *in;
    size_t pos, end;
    u32 range, code;
    int bad;
} rc_t;

typedef struct {
    u16 choice, choice2;
    u16 low[LZ_POS][8];
    u16 mid[LZ_POS][8];
    u16 high[256];
} lz_len_t;

typedef struct {
    u32 lc, lp, pb;
    u32 state, rep[4];
    u16 lit[LZ_LIT];
    u16 is_match[LZ_STATES][LZ_POS];
    u16 is_rep[LZ_STATES], is_rep0[LZ_STATES], is_rep1[LZ_STATES], is_rep2[LZ_STATES];
    u16 is_rep0_long[LZ_STATES][LZ_POS];
    u16 slot[4][64];
    u16 spec[115];
    u16 align[16];
    lz_len_t len, rep_len;
} lz_t;

static void fill(u16 *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (u16)(LZ_TOTAL / 2);
}

static void lz_reset(lz_t *z) {
    z->state = 0;
    z->rep[0] = z->rep[1] = z->rep[2] = z->rep[3] = 0;
    fill(z->lit, LZ_LIT);
    fill(&z->is_match[0][0], LZ_STATES * LZ_POS);
    fill(z->is_rep, LZ_STATES);
    fill(z->is_rep0, LZ_STATES);
    fill(z->is_rep1, LZ_STATES);
    fill(z->is_rep2, LZ_STATES);
    fill(&z->is_rep0_long[0][0], LZ_STATES * LZ_POS);
    fill(&z->slot[0][0], 4 * 64);
    fill(z->spec, 115);
    fill(z->align, 16);
    fill((u16 *)&z->len, sizeof z->len / sizeof(u16));
    fill((u16 *)&z->rep_len, sizeof z->rep_len / sizeof(u16));
}

static int rc_init(rc_t *rc) {
    if (rc->end - rc->pos < 5 || rc->in[rc->pos] != 0) return 0;
    rc->range = 0xFFFFFFFFu;
    rc->code = 0;
    for (int i = 1; i < 5; i++) rc->code = (rc->code << 8) | rc->in[rc->pos + i];
    rc->pos += 5;
    rc->bad = 0;
    return rc->code != rc->range;
}

static inline void rc_norm(rc_t *rc) {
    if (rc->range < (1u << 24)) {
        rc->range <<= 8;
        if (rc->pos < rc->end) rc->code = (rc->code << 8) | rc->in[rc->pos++];
        else { rc->code <<= 8; rc->bad = 1; }
    }
}

static inline u32 rc_bit(rc_t *rc, u16 *p) {
    u32 bound = (rc->range >> LZ_BITS) * *p;
    u32 b;
    if (rc->code < bound) {
        rc->range = bound;
        *p = (u16)(*p + ((LZ_TOTAL - *p) >> LZ_MOVE));
        b = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *p = (u16)(*p - (*p >> LZ_MOVE));
        b = 1;
    }
    rc_norm(rc);
    return b;
}

static u32 rc_direct(rc_t *rc, u32 n) {
    u32 v = 0;
    while (n--) {
        rc->range >>= 1;
        u32 b = rc->code >= rc->range;
        if (b) rc->code -= rc->range;
        v = (v << 1) | b;
        rc_norm(rc);
    }
    return v;
}

static u32 rc_tree(rc_t *rc, u16 *p, u32 bits) {
    u32 m = 1;
    for (u32 i = 0; i < bits; i++) m = (m << 1) | rc_bit(rc, &p[m]);
    return m - (1u << bits);
}

static u32 rc_rtree(rc_t *rc, u16 *p, u32 bits) {
    u32 m = 1, v = 0;
    for (u32 i = 0; i < bits; i++) {
        u32 b = rc_bit(rc, &p[m]);
        m = (m << 1) | b;
        v |= b << i;
    }
    return v;
}

static u32 len_dec(rc_t *rc, lz_len_t *l, u32 ps) {
    if (!rc_bit(rc, &l->choice)) return rc_tree(rc, l->low[ps], 3);
    if (!rc_bit(rc, &l->choice2)) return 8 + rc_tree(rc, l->mid[ps], 3);
    return 16 + rc_tree(rc, l->high, 8);
}

static u32 dist_dec(rc_t *rc, lz_t *z, u32 len) {
    u32 ls = len < 3 ? len : 3;
    u32 slot = rc_tree(rc, z->slot[ls], 6);
    if (slot < 4) return slot;
    u32 nd = (slot >> 1) - 1;
    u32 d = (2u | (slot & 1u)) << nd;
    if (slot < 14) return d + rc_rtree(rc, z->spec + d - slot, nd);
    d += rc_direct(rc, nd - 4) << 4;
    return d + rc_rtree(rc, z->align, 4);
}

static int lz_chunk(lz_t *z, rc_t *rc, u8 *out, size_t *op, size_t lim, size_t base) {
    size_t o = *op;
    u32 pbm = (1u << z->pb) - 1, lpm = (1u << z->lp) - 1;
    while (o < lim) {
        u32 pos = (u32)(o - base);
        u32 ps = pos & pbm;
        u32 st = z->state;
        if (!rc_bit(rc, &z->is_match[st][ps])) {
            u32 prev = o > base ? out[o - 1] : 0;
            u16 *p = z->lit + 0x300u * (((pos & lpm) << z->lc) + (prev >> (8 - z->lc)));
            u32 sym = 1;
            if (st >= 7) {
                if ((size_t)z->rep[0] + 1 > o - base) return 0;
                u32 mb = out[o - z->rep[0] - 1];
                do {
                    u32 mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    u32 b = rc_bit(rc, &p[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (mbit != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | rc_bit(rc, &p[sym]);
            out[o++] = (u8)(sym - 0x100);
            z->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
            continue;
        }
        u32 len;
        if (!rc_bit(rc, &z->is_rep[st])) {
            z->rep[3] = z->rep[2];
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            len = len_dec(rc, &z->len, ps);
            z->state = st < 7 ? 7 : 10;
            z->rep[0] = dist_dec(rc, z, len);
            if (z->rep[0] == 0xFFFFFFFFu) return 0;
        } else {
            if (o == base) return 0;
            if (!rc_bit(rc, &z->is_rep0[st])) {
                if (!rc_bit(rc, &z->is_rep0_long[st][ps])) {
                    z->state = st < 7 ? 9 : 11;
                    if ((size_t)z->rep[0] + 1 > o - base) return 0;
                    out[o] = out[o - z->rep[0] - 1];
                    o++;
                    continue;
                }
            } else {
                u32 d;
                if (!rc_bit(rc, &z->is_rep1[st])) {
                    d = z->rep[1];
                } else {
                    if (!rc_bit(rc, &z->is_rep2[st])) {
                        d = z->rep[2];
                    } else {
                        d = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = d;
            }
            len = len_dec(rc, &z->rep_len, ps);
            z->state = st < 7 ? 8 : 11;
        }
        len += 2;
        size_t dist = (size_t)z->rep[0] + 1;
        if (dist > o - base || o + len > lim) return 0;
        u8 *dst = out + o;
        const u8 *src = dst - dist;
        for (u32 i = 0; i < len; i++) dst[i] = src[i];
        o += len;
        if (rc->bad) return 0;
    }
    *op = o;
    return !rc->bad;
}

static int lz2_block(const u8 *in, size_t len, size_t *ip, u8 *out, size_t out_len, size_t *op,
                     lz_t *z, size_t *base, int *have_props) {
    size_t i = *ip, o = *op;
    while (i < len) {
        u32 c = in[i++];
        if (c == 0x00) {
            *ip = i;
            *op = o;
            return 2;
        }
        if (c == 0x01 || c == 0x02) {
            if (i + 2 > len) return 0;
            size_t n = (((size_t)in[i] << 8) | in[i + 1]) + 1;
            i += 2;
            if (i + n > len || o + n > out_len) return 0;
            if (c == 0x01) *base = o;
            memcpy(out + o, in + i, n);
            i += n;
            o += n;
            continue;
        }
        if (c < 0x80 || i + 4 > len) return 0;
        size_t un = ((size_t)(c & 0x1Fu) << 16) + ((size_t)in[i] << 8) + in[i + 1] + 1;
        size_t cn = ((size_t)in[i + 2] << 8) + in[i + 3] + 1;
        i += 4;
        u32 mode = (c >> 5) & 3u;
        if (mode == 3) *base = o;
        if (mode >= 2) {
            if (i >= len) return 0;
            u32 pr = in[i++];
            if (pr >= 9 * 5 * 5) return 0;
            z->lc = pr % 9;
            pr /= 9;
            z->lp = pr % 5;
            z->pb = pr / 5;
            if (z->lc + z->lp > 4) return 0;
            *have_props = 1;
        }
        if (!*have_props) return 0;
        if (mode >= 1) lz_reset(z);
        if (i + cn > len || o + un > out_len) return 0;
        rc_t rc = { in, i, i + cn, 0, 0, 0 };
        if (!rc_init(&rc)) return 0;
        if (!lz_chunk(z, &rc, out, &o, o + un, *base)) return 0;
        i += cn;
    }
    *ip = i;
    *op = o;
    return 1;
}

static int xz_varint(const u8 *p, size_t n, size_t *k, u64 *v) {
    *v = 0;
    for (int s = 0; s < 63; s += 7) {
        if (*k >= n) return 0;
        u8 b = p[(*k)++];
        *v |= (u64)(b & 0x7F) << s;
        if (!(b & 0x80)) return 1;
    }
    return 0;
}

int ac3_xz_decode(const u8 *in, size_t in_len, u8 *out, size_t out_len, size_t *produced) {
    static const u8 magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
    *produced = 0;
    if (in_len < 12 || memcmp(in, magic, 6) != 0 || in[6] != 0 || (in[7] & 0xF0)) return 0;
    u32 check = in[7] & 0x0Fu;
    size_t check_size = check ? (size_t)4 << ((check - 1) / 3) : 0;
    lz_t *z = calloc(1, sizeof *z);
    if (!z) return 0;
    size_t i = 12, o = 0, base = 0;
    int have_props = 0, ok = 1;
    while (i < in_len) {
        if (in[i] == 0) break;
        size_t bs = i;
        size_t hs = ((size_t)in[i] + 1) * 4;
        if (i + hs > in_len) { ok = 0; break; }
        const u8 *h = in + i;
        u32 flags = h[1];
        if (flags & 0x3C) { ok = 0; break; }
        size_t k = 2;
        u64 v, id, ps;
        if ((flags & 0x40) && !xz_varint(h, hs - 4, &k, &v)) { ok = 0; break; }
        if ((flags & 0x80) && !xz_varint(h, hs - 4, &k, &v)) { ok = 0; break; }
        if ((flags & 3) != 0 || !xz_varint(h, hs - 4, &k, &id) || !xz_varint(h, hs - 4, &k, &ps) ||
            id != 0x21 || ps != 1 || k + 1 > hs - 4) { ok = 0; break; }
        i += hs;
        int r = lz2_block(in, in_len, &i, out, out_len, &o, z, &base, &have_props);
        if (r == 0) { ok = 0; break; }
        if (r == 1) break;
        while ((i - bs) & 3) i++;
        i += check_size;
    }
    free(z);
    *produced = o;
    return ok;
}
