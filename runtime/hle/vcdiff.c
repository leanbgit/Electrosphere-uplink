#include "vcdiff.h"
#include "xz_dec.h"
#include <stdlib.h>
#include <string.h>

enum { VT_NOOP, VT_ADD, VT_RUN, VT_COPY };

typedef struct { u8 type, size, mode; } vcd_half_t;
typedef struct { vcd_half_t h[2]; } vcd_code_t;

static vcd_code_t table[256];
static int table_ready;

static void build_table(void) {
    int n = 0;
    table[n].h[0] = (vcd_half_t){ VT_RUN, 0, 0 }; n++;
    table[n].h[0] = (vcd_half_t){ VT_ADD, 0, 0 }; n++;
    for (int s = 1; s <= 17; s++) { table[n].h[0] = (vcd_half_t){ VT_ADD, (u8)s, 0 }; n++; }
    for (int m = 0; m < 9; m++) {
        table[n].h[0] = (vcd_half_t){ VT_COPY, 0, (u8)m }; n++;
        for (int s = 4; s <= 18; s++) { table[n].h[0] = (vcd_half_t){ VT_COPY, (u8)s, (u8)m }; n++; }
    }
    for (int m = 0; m < 6; m++)
        for (int a = 1; a <= 4; a++)
            for (int c = 4; c <= 6; c++) {
                table[n].h[0] = (vcd_half_t){ VT_ADD, (u8)a, 0 };
                table[n].h[1] = (vcd_half_t){ VT_COPY, (u8)c, (u8)m };
                n++;
            }
    for (int m = 6; m < 9; m++)
        for (int a = 1; a <= 4; a++) {
            table[n].h[0] = (vcd_half_t){ VT_ADD, (u8)a, 0 };
            table[n].h[1] = (vcd_half_t){ VT_COPY, 4, (u8)m };
            n++;
        }
    for (int m = 0; m < 9; m++) {
        table[n].h[0] = (vcd_half_t){ VT_COPY, 4, (u8)m };
        table[n].h[1] = (vcd_half_t){ VT_ADD, 1, 0 };
        n++;
    }
    table_ready = 1;
}

static int varint(const u8 *b, size_t n, size_t *p, u64 *v) {
    *v = 0;
    for (int i = 0; i < 10; i++) {
        if (*p >= n) return 0;
        u8 c = b[(*p)++];
        *v = (*v << 7) | (c & 0x7Fu);
        if (!(c & 0x80)) return 1;
    }
    return 0;
}

u32 vcd_adler32(const u8 *p, size_t n) {
    return vcd_adler32_more(1, p, n);
}

u32 vcd_adler32_more(u32 sum, const u8 *p, size_t n) {
    u32 a = sum & 0xFFFFu, b = sum >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k >= 8) {
            a += p[0]; b += a; a += p[1]; b += a; a += p[2]; b += a; a += p[3]; b += a;
            a += p[4]; b += a; a += p[5]; b += a; a += p[6]; b += a; a += p[7]; b += a;
            p += 8;
            k -= 8;
        }
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

typedef struct {
    const u8 *raw;
    size_t raw_len;
    int comp;
} vcd_raw_t;

void vcd_free(vcd_patch_t *p) {
    free(p->win);
    for (int k = 0; k < 3; k++) free(p->owned[k]);
    memset(p, 0, sizeof *p);
}

int vcd_parse(const u8 *b, size_t n, vcd_patch_t *out) {
    memset(out, 0, sizeof *out);
    if (!table_ready) build_table();
    if (n < 5 || b[0] != 0xD6 || b[1] != 0xC3 || b[2] != 0xC4 || b[3] != 0) return 0;
    u8 hi = b[4];
    size_t p = 5;
    if (hi & 1) {
        if (p >= n || b[p] != 2) return 0;
        p++;
    }
    if (hi & 2) return 0;
    if (hi & 4) {
        u64 alen;
        if (!varint(b, n, &p, &alen) || alen > n - p) return 0;
        size_t c = alen < sizeof out->app - 1 ? (size_t)alen : sizeof out->app - 1;
        memcpy(out->app, b + p, c);
        out->app[c] = 0;
        p += (size_t)alen;
    }
    int cap = 0;
    vcd_raw_t (*raw)[3] = NULL;
    int ok = 1;
    while (p < n) {
        if (out->nwin == cap) {
            cap = cap ? cap * 2 : 64;
            vcd_window_t *w2 = realloc(out->win, (size_t)cap * sizeof *w2);
            if (w2) out->win = w2;
            vcd_raw_t (*r2)[3] = realloc(raw, (size_t)cap * sizeof *r2);
            if (r2) raw = r2;
            if (!w2 || !r2) { ok = 0; break; }
        }
        vcd_window_t *w = &out->win[out->nwin];
        memset(w, 0, sizeof *w);
        u8 wi = b[p++];
        if ((wi & 0xF8) || (wi & 2)) { ok = 0; break; }
        u64 dlen, tlen, lens[3];
        if (wi & 1) {
            if (!varint(b, n, &p, &w->src_len) || !varint(b, n, &p, &w->src_pos)) { ok = 0; break; }
        }
        if (!varint(b, n, &p, &dlen) || dlen > n - p) { ok = 0; break; }
        size_t start = p;
        if (!varint(b, n, &p, &tlen) || tlen > 0x7FFFFFFFu || p >= n) { ok = 0; break; }
        u8 di = b[p++];
        for (int k = 0; k < 3; k++)
            if (!varint(b, n, &p, &lens[k])) { ok = 0; break; }
        if (!ok) break;
        if (wi & 4) {
            if (p + 4 > n) { ok = 0; break; }
            w->has_adler = 1;
            w->adler = (u32)b[p] << 24 | (u32)b[p + 1] << 16 | (u32)b[p + 2] << 8 | b[p + 3];
            p += 4;
        }
        for (int k = 0; k < 3; k++) {
            if (lens[k] > n - p) { ok = 0; break; }
            raw[out->nwin][k].raw = b + p;
            raw[out->nwin][k].raw_len = (size_t)lens[k];
            raw[out->nwin][k].comp = (di >> k) & 1;
            p += (size_t)lens[k];
        }
        if (!ok || p != start + dlen) { ok = 0; break; }
        w->tlen = (u32)tlen;
        w->tpos = out->target_size;
        out->target_size += tlen;
        if (w->tlen > out->max_tlen) out->max_tlen = w->tlen;
        out->nwin++;
    }
    for (int k = 0; k < 3 && ok; k++) {
        size_t comp_total = 0, dec_total = 0;
        for (int i = 0; i < out->nwin && ok; i++) {
            vcd_raw_t *r = &raw[i][k];
            if (!r->comp) continue;
            size_t q = 0;
            u64 dsz;
            if (!varint(r->raw, r->raw_len, &q, &dsz)) { ok = 0; break; }
            comp_total += r->raw_len - q;
            dec_total += (size_t)dsz;
        }
        if (!ok) break;
        u8 *cbuf = NULL, *dbuf = NULL;
        if (comp_total) {
            cbuf = malloc(comp_total);
            dbuf = malloc(dec_total ? dec_total : 1);
            if (!cbuf || !dbuf) { free(cbuf); free(dbuf); ok = 0; break; }
            size_t c = 0;
            for (int i = 0; i < out->nwin; i++) {
                vcd_raw_t *r = &raw[i][k];
                if (!r->comp) continue;
                size_t q = 0;
                u64 dsz;
                varint(r->raw, r->raw_len, &q, &dsz);
                memcpy(cbuf + c, r->raw + q, r->raw_len - q);
                c += r->raw_len - q;
            }
            size_t got = 0;
            if (!ac3_xz_decode(cbuf, comp_total, dbuf, dec_total, &got) || got != dec_total) {
                free(cbuf);
                free(dbuf);
                ok = 0;
                break;
            }
            free(cbuf);
            out->owned[k] = dbuf;
        }
        size_t d = 0;
        for (int i = 0; i < out->nwin; i++) {
            vcd_raw_t *r = &raw[i][k];
            const u8 *ptr;
            size_t len;
            if (r->comp) {
                size_t q = 0;
                u64 dsz;
                varint(r->raw, r->raw_len, &q, &dsz);
                ptr = dbuf + d;
                len = (size_t)dsz;
                d += len;
            } else {
                ptr = r->raw;
                len = r->raw_len;
            }
            vcd_window_t *w = &out->win[i];
            if (k == 0) { w->data = ptr; w->data_len = len; }
            else if (k == 1) { w->inst = ptr; w->inst_len = len; }
            else { w->addr = ptr; w->addr_len = len; }
        }
    }
    free(raw);
    if (!ok) vcd_free(out);
    return ok;
}

int vcd_apply_window(const vcd_window_t *w, const u8 *src, u8 *t, vcd_copy_fn fn, void *user) {
    if (!table_ready) build_table();
    u64 near[4] = { 0, 0, 0, 0 };
    u64 same[3 * 256];
    memset(same, 0, sizeof same);
    int nslot = 0;
    size_t dp = 0, ip = 0, ap = 0;
    u64 pos = 0, S = w->src_len;
    while (ip < w->inst_len) {
        const vcd_code_t *c = &table[w->inst[ip++]];
        for (int hh = 0; hh < 2; hh++) {
            const vcd_half_t *h = &c->h[hh];
            if (h->type == VT_NOOP) continue;
            u64 size = h->size;
            if (!size && !varint(w->inst, w->inst_len, &ip, &size)) return 0;
            if (pos + size > w->tlen) return 0;
            if (h->type == VT_ADD) {
                if (dp + size > w->data_len) return 0;
                memcpy(t + pos, w->data + dp, (size_t)size);
                dp += (size_t)size;
            } else if (h->type == VT_RUN) {
                if (dp >= w->data_len) return 0;
                memset(t + pos, w->data[dp++], (size_t)size);
            } else {
                u64 here = S + pos, a, v;
                u8 m = h->mode;
                if (m == 0) {
                    if (!varint(w->addr, w->addr_len, &ap, &a)) return 0;
                } else if (m == 1) {
                    if (!varint(w->addr, w->addr_len, &ap, &v) || v > here) return 0;
                    a = here - v;
                } else if (m < 6) {
                    if (!varint(w->addr, w->addr_len, &ap, &v)) return 0;
                    a = near[m - 2] + v;
                } else {
                    if (ap >= w->addr_len) return 0;
                    a = same[(m - 6) * 256 + w->addr[ap++]];
                }
                near[nslot] = a;
                nslot = (nslot + 1) & 3;
                same[a % (3 * 256)] = a;
                if (a >= here) return 0;
                u64 left = size, to = pos;
                if (a < S) {
                    u64 k = S - a < left ? S - a : left;
                    memcpy(t + to, src + a, (size_t)k);
                    if (fn) fn(user, w->tpos + to, (u32)k, w->src_pos + a);
                    to += k;
                    left -= k;
                    a += k;
                }
                if (left) {
                    u64 ta = a - S;
                    u8 *dst = t + to;
                    const u8 *from = t + ta;
                    if (ta + left <= to) memcpy(dst, from, (size_t)left);
                    else for (u64 i = 0; i < left; i++) dst[i] = from[i];
                }
            }
            pos += size;
        }
    }
    return pos == w->tlen && dp == w->data_len && ap == w->addr_len;
}
