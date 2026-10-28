/* coop_probe.c -- see coop_probe.h. */
#ifdef AC3_COOP_PROBE
#include "coop_probe.h"
#include "hle.h"
#include "ac3_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GAME_GP 0x800BCB74u

extern u32 ac3_vblank_count(void);
extern u32 ac3_interp_pc;
extern u32 ac3_guest_ra;
extern u16 *ac3_vram;
extern u32 ac3_watch_lo, ac3_watch_hi;

static u64 fnv(const void *p, size_t n, u64 h) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
static u64 hash64(const void *p, size_t n) {      /* fast: 8 bytes per step */
    const u8 *b = (const u8 *)p;
    u64 h = 0x9E3779B97F4A7C15ull ^ (u64)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w; memcpy(&w, b + i, 8);
        h ^= w; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 32;
    }
    return fnv(b + i, n - i, h);
}

static int range(const char *s, u32 *lo, u32 *hi, u32 *every) {
    char *e;
    *lo = (u32)strtoul(s, &e, 0);
    *hi = *lo;
    if (*e == '-') *hi = (u32)strtoul(e + 1, &e, 0);
    if (every) { *every = 1; if (*e == ':') *every = (u32)strtoul(e + 1, &e, 0); if (!*every) *every = 1; }
    return *hi >= *lo;
}

static FILE *open_w(const char *p) {
    FILE *f = fopen(p, "w");
    if (!f) fprintf(stderr, "coop-probe: cannot write %s\n", p);
    return f;
}

static struct {
    FILE *hash;
    FILE *ents; u32 e_lo, e_hi, e_every;
    u32 ram_at[64]; int ram_n; char ram_dir[512];
    FILE *wlog; u32 w_lo, w_hi;
    FILE *rlog; u32 rf_lo, rf_hi;
    FILE *calls; u32 c_lo, c_hi;
    FILE *p2; int p2_done;
    u32 fn[16]; int nfn; u32 fn_lo, fn_hi; FILE *fnlog;
} P;
static u32 w_addr_lo, w_addr_hi;
u32 ac3_coop_r_lo, ac3_coop_r_hi;
static u32 r_addr_lo, r_addr_hi;

int ac3_coop_probe_arg(int argc, char **argv, int *j) {
    const char *a = argv[*j];
    int left = argc - *j - 1;
    if (!strcmp(a, "--coop-hash") && left >= 1) {
        P.hash = open_w(argv[++*j]);
        if (P.hash) fprintf(P.hash, "field\tgroup\tphase\tmission\tram\tscratch\tvram\tspu\n");
        return 1;
    }
    if (!strcmp(a, "--coop-ents") && left >= 2) {
        range(argv[*j + 1], &P.e_lo, &P.e_hi, &P.e_every);
        P.ents = open_w(argv[*j + 2]);
        *j += 2; return 1;
    }
    if (!strcmp(a, "--coop-ram") && left >= 2) {
        char *s = argv[*j + 1];
        while (*s && P.ram_n < 64) { P.ram_at[P.ram_n++] = (u32)strtoul(s, &s, 0); if (*s == ',') s++; else break; }
        snprintf(P.ram_dir, sizeof P.ram_dir, "%s", argv[*j + 2]);
        mkdir(P.ram_dir, 0755);
        *j += 2; return 1;
    }
    if ((!strcmp(a, "--coop-wlog") || !strcmp(a, "--coop-rlog")) && left >= 4) {
        u32 lo = (u32)strtoul(argv[*j + 1], NULL, 0);
        u32 len = (u32)strtoul(argv[*j + 2], NULL, 0);
        if (a[7] == 'w') {
            w_addr_lo = lo; w_addr_hi = lo + len;
            range(argv[*j + 3], &P.w_lo, &P.w_hi, NULL);
            P.wlog = open_w(argv[*j + 4]);
            if (P.wlog) fprintf(P.wlog, "field\tpc\tcaller\taddr\tsize\tvalue\n");
        } else {
            r_addr_lo = lo; r_addr_hi = lo + len;
            range(argv[*j + 3], &P.rf_lo, &P.rf_hi, NULL);
            P.rlog = open_w(argv[*j + 4]);
            if (P.rlog) fprintf(P.rlog, "field\tpc\tcaller\taddr\tsize\tvalue\n");
        }
        *j += 4; return 1;
    }
    if (!strcmp(a, "--coop-calls") && left >= 2) {
        range(argv[*j + 1], &P.c_lo, &P.c_hi, NULL);
        P.calls = open_w(argv[*j + 2]);
        if (P.calls) fprintf(P.calls, "field\ttarget\tcaller\n");
        *j += 2; return 1;
    }
    if (!strcmp(a, "--coop-args") && left >= 3) {
        char *s = argv[*j + 1];
        while (*s && P.nfn < 16) { P.fn[P.nfn++] = (u32)strtoul(s, &s, 0); if (*s == ',') s++; else break; }
        range(argv[*j + 2], &P.fn_lo, &P.fn_hi, NULL);
        P.fnlog = open_w(argv[*j + 3]);
        if (P.fnlog) fprintf(P.fnlog, "field\ttarget\tcaller\ta0\ta1\ta2\ta3\tsp\n");
        *j += 3; return 1;
    }
    if (!strcmp(a, "--coop-p2") && left >= 1) {
        P.p2 = fopen(argv[++*j], "rb");
        if (!P.p2) { fprintf(stderr, "coop-probe: cannot read %s\n", argv[*j]); return 1; }
        char head[16];
        if (fread(head, 1, 16, P.p2) != 16) fprintf(stderr, "coop-probe: %s too short\n", argv[*j]);
        return 1;
    }
    return 0;
}

/* --- per-field ---------------------------------------------------------- */

extern void ac3_spu_state_hash(u64 *h);   /* psyq_spu.c (probe builds) */

static void field_hash(u32 f) {
    u32 scene = MEM_R32(GAME_GP + 748);
    int ok = (scene >> 24) == 0x80u && (scene & 0x1FFFFFu) < AC3_RAM_SIZE - 1024;
    u64 spu = 0;
    ac3_spu_state_hash(&spu);
    fprintf(P.hash, "%u\t%d\t%d\t%d\t%016llx\t%016llx\t%016llx\t%016llx\n", f,
            ok ? (int)MEM_R32(scene + 44) : -1, ok ? (int)MEM_R32(scene + 48) : -1,
            ok ? (int)MEM_R16S(scene + 908) : -1,
            (unsigned long long)hash64(ac3_ram, AC3_RAM_SIZE),
            (unsigned long long)hash64(ac3_scratch, 1024),
            (unsigned long long)(ac3_vram ? hash64(ac3_vram, 1024 * 512 * 2) : 0),
            (unsigned long long)spu);
}

static void field_ents(u32 f) {
    u32 slots = MEM_R32(GAME_GP + 744);
    extern u32 ac3_coop_pad_buffer(int port);
    fprintf(P.ents, "== field %u  slots 0x%08X  world 0x%08X  scene 0x%08X  pad1 0x%08X  pad2 0x%08X\n",
            f, slots, MEM_R32(GAME_GP + 736), MEM_R32(GAME_GP + 748),
            ac3_coop_pad_buffer(0), ac3_coop_pad_buffer(1));
    if ((slots >> 24) != 0x80u) return;
    for (u32 i = 0; i < 64; i++) {
        u32 e = MEM_R32(slots + 4 * i);
        if ((e >> 24) != 0x80u || (e & 0x1FFFFFu) + 0x100 > AC3_RAM_SIZE) continue;
        fprintf(P.ents, "slot %2u 0x%08X:", i, e);
        for (u32 k = 0; k < 0x100; k += 4) fprintf(P.ents, " %08X", MEM_R32(e + k));
        fputc('\n', P.ents);
    }
}

static void field_ram(u32 f) {
    for (int i = 0; i < P.ram_n; i++) {
        if (P.ram_at[i] != f) continue;
        char p[600];
        snprintf(p, sizeof p, "%s/ram_%06u.bin", P.ram_dir, f);
        FILE *o = fopen(p, "wb");
        if (o) { fwrite(ac3_ram, 1, AC3_RAM_SIZE, o); fwrite(ac3_scratch, 1, 1024, o); fclose(o); }
    }
}

/* Port 2 from a second recording (24-byte records: field, pad1[10], pad2[10]):
 * lets the bench drive two controllers while the main recording keeps port 1. */
void ac3_coop_probe_pads(void) {
    if (!P.p2 || P.p2_done) return;
    u8 r[24];
    if (fread(r, 1, sizeof r, P.p2) != sizeof r) { P.p2_done = 1; return; }
    const u8 *p = r + 14;
    ac3_pad[1].connected = p[0];
    ac3_pad[1].type = p[1];
    ac3_pad[1].buttons = (u16)(p[2] | (p[3] << 8));
    ac3_pad[1].lx = p[4]; ac3_pad[1].ly = p[5];
    ac3_pad[1].rx = p[6]; ac3_pad[1].ry = p[7];
}

void ac3_coop_probe_field(void) {
    u32 f = ac3_vblank_count();
    if (P.hash) field_hash(f);
    if (P.ents && f >= P.e_lo && f <= P.e_hi && (f - P.e_lo) % P.e_every == 0) field_ents(f);
    if (P.ram_n) field_ram(f);
    /* write and read logs are armed only inside their field windows */
    if (P.wlog) {
        int on = f >= P.w_lo && f <= P.w_hi;
        ac3_watch_lo = on ? w_addr_lo : 0;
        ac3_watch_hi = on ? w_addr_hi : 0;
        if (f > P.w_hi) { fclose(P.wlog); P.wlog = NULL; }
    }
    if (P.rlog) {
        int on = f >= P.rf_lo && f <= P.rf_hi;
        ac3_coop_r_lo = on ? r_addr_lo : 0;
        ac3_coop_r_hi = on ? r_addr_hi : 0;
        if (f > P.rf_hi) { fclose(P.rlog); P.rlog = NULL; }
    }
    if (P.calls && f > P.c_hi) { fclose(P.calls); P.calls = NULL; }
    if (f % 600 == 0) {
        FILE *all[] = { P.hash, P.ents, P.wlog, P.rlog, P.calls };
        for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) if (all[i]) fflush(all[i]);
    }
}

void ac3_coop_write_hit(u32 addr, u32 value, int size) {
    if (!P.wlog) return;
    fprintf(P.wlog, "%u\t%08X\t%08X\t%08X\t%d\t%08X\n", ac3_vblank_count(),
            ac3_interp_pc, ac3_guest_ra, addr, size, value);
}

void ac3_coop_read_hit(u32 addr, int size) {
    if (!P.rlog) return;
    u32 v = size == 4 ? MEM_R32(addr) : size == 2 ? MEM_R16U(addr) : MEM_R8U(addr);
    fprintf(P.rlog, "%u\t%08X\t%08X\t%08X\t%d\t%08X\n", ac3_vblank_count(),
            ac3_interp_pc, ac3_guest_ra, addr, size, v);
}

void ac3_coop_call_args(ctx_t *ctx, u32 target) {
    if (!P.fnlog) return;
    u32 f = ac3_vblank_count();
    if (f < P.fn_lo || f > P.fn_hi) return;
    for (int i = 0; i < P.nfn; i++)
        if (P.fn[i] == target)
            fprintf(P.fnlog, "%u\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\n", f, target, ac3_guest_ra,
                    ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7], ctx->r[29]);
}

void ac3_coop_call(u32 target) {
    if (!P.calls) return;
    u32 f = ac3_vblank_count();
    if (f < P.c_lo || f > P.c_hi) return;
    fprintf(P.calls, "%u\t%08X\t%08X\n", f, target, ac3_guest_ra);
}

void ac3_coop_probe_finish(void) {
    FILE *all[] = { P.hash, P.ents, P.wlog, P.rlog, P.calls, P.p2, P.fnlog };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) if (all[i]) fclose(all[i]);
    memset(&P, 0, sizeof P);
}
#endif
