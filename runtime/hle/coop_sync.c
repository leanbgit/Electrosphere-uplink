/* coop_sync.c -- see coop_sync.h. */
#include "coop_sync.h"
#include "gpu_sw.h"
#include "hle.h"
#include <string.h>

#define GAME_GP 0x800BCB74u

extern u32 ac3_vblank_count(void);
extern u16 *ac3_vram;
extern void ac3_spu_state_hash(u64 *h);   /* psyq_spu.c */

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

/* RAM without the two picture buffers (OT + drawing packets), which the
 * guest's view legitimately changes: what the two computers must agree on. */
u64 ac3_coop_sync_sim(void) {
    u32 w = MEM_R32(GAME_GP + 0x2E0u);
    u32 lo = 0, hi = 0;
    if ((w >> 24) == 0x80u && (w & 0x1FFFFFu) < AC3_RAM_SIZE - 0x40u) {
        u32 ot0 = MEM_R32(w + 0x18u), ot1 = MEM_R32(w + 0x1Cu), pk0 = MEM_R32(w + 0x20u), pk1 = MEM_R32(w + 0x24u);
        if (ot0 < ot1 && pk0 < ot1 && pk1 > ot1 && (ot0 >> 24) == 0x80u) {
            lo = ot0 & AC3_RAM_MASK;
            hi = (pk1 + (ot1 - pk0)) & AC3_RAM_MASK;
            if (hi <= lo || hi > AC3_RAM_SIZE) lo = hi = 0;
        }
    }
    u64 h = hash64(ac3_ram, lo ? lo : AC3_RAM_SIZE);
    if (lo) h ^= hash64(ac3_ram + hi, AC3_RAM_SIZE - hi) * 31u;
    if (ac3_ext) h ^= hash64(ac3_ext, AC3_EXT_SIZE) * 7u;   /* the extra memory (the guest's plane) */
    return h;
}

void ac3_coop_sync_header(FILE *f) {
    fprintf(f, "field\tgroup\tphase\tmission\tram\tscratch\tvram\tspu\tsim\n");
}

void ac3_coop_sync_row(FILE *f, u32 field) {
    u32 scene = MEM_R32(GAME_GP + 748);
    int ok = (scene >> 24) == 0x80u && (scene & 0x1FFFFFu) < AC3_RAM_SIZE - 1024;
    u64 spu = 0;
    ac3_spu_state_hash(&spu);
    fprintf(f, "%u\t%d\t%d\t%d\t%016llx\t%016llx\t%016llx\t%016llx\t%016llx\n", field,
            ok ? (int)MEM_R32(scene + 44) : -1, ok ? (int)MEM_R32(scene + 48) : -1,
            ok ? (int)MEM_R16S(scene + 908) : -1,
            (unsigned long long)(hash64(ac3_ram, AC3_RAM_SIZE) ^ (ac3_ext ? hash64(ac3_ext, AC3_EXT_SIZE) * 7u : 0)),
            (unsigned long long)hash64(ac3_scratch, 1024),
            (unsigned long long)((ac3_vram ? hash64(ac3_vram, 1024 * 512 * 2) : 0) ^
                                 (ac3_vram_bank ? hash64(ac3_vram_bank, 1024 * 512 * 2) * 7u : 0)),   /* + the second bank */
            (unsigned long long)spu, (unsigned long long)ac3_coop_sync_sim());
}

static FILE *log_file;

int ac3_coop_sync_log_open(const char *path) {
    log_file = fopen(path, "w");
    if (!log_file) { fprintf(stderr, "coop: cannot write %s\n", path); return 0; }
    ac3_coop_sync_header(log_file);
    printf("coop: state fingerprints every field -> %s\n", path);
    return 1;
}

void ac3_coop_sync_field(void) {
    if (log_file) ac3_coop_sync_row(log_file, ac3_vblank_count());
}

void ac3_coop_sync_log_close(void) {
    if (log_file) { fclose(log_file); log_file = NULL; }
}
