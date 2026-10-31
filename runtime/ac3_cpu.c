#include "ac3_runtime.h"
#include "hle/ac3_stream_probe.h"
#include "hle/ac3_oracle.h"
#include <stdio.h>
#include <stdlib.h>

u32 ac3_cur_func = 0;
u8 *ac3_ram = NULL;
u8 *ac3_scratch = NULL;
u8 *ac3_ext = NULL;

void ac3_ext_enable(void) {
    if (!ac3_ext) ac3_ext = (u8 *)calloc(1, AC3_EXT_SIZE);
}

extern const struct { u32 addr; ac3_func_t fn; } ac3_func_table[];
extern const unsigned ac3_func_table_len;

void ac3_mem_init(void) {
    ac3_ram = (u8 *)calloc(1, AC3_RAM_SIZE);
    ac3_scratch = (u8 *)calloc(1, 1024);
    if (!ac3_ram || !ac3_scratch) {
        fprintf(stderr, "ac3: out of memory\n");
        exit(1);
    }
}

static int io_warned[0x2000 / 4];

u32 ac3_mem_read_slow(u32 addr, int size) {
    u32 p = ac3_phys(addr);
    u8 *x = ac3_ext_ptr(p, (u32)size);
    if (x) {
        u32 v = 0;
        for (int i = 0; i < size; i++) v |= (u32)x[i] << (8 * i);
        return v;
    }
    if (p >= 0x1F800000u && p < 0x1F800400u) {
        u8 *s = ac3_scratch + (p - 0x1F800000u);
        u32 v = 0;
        for (int i = 0; i < size; i++) v |= (u32)s[i] << (8 * i);
        return v;
    }
    if (p >= 0x1F801000u && p < 0x1F803000u) {
        unsigned idx = (p - 0x1F801000u) / 4;
        if (idx < sizeof(io_warned) / sizeof(io_warned[0]) && !io_warned[idx]) {
            io_warned[idx] = 1;
            fprintf(stderr, "ac3: unhandled I/O read  0x%08X (%d)\n", addr, size);
        }
        return 0;
    }
    fprintf(stderr, "ac3: bad read 0x%08X (%d) in func 0x%08X\n",
            addr, size, ac3_cur_func);
    return 0;
}

void ac3_mem_write_slow(u32 addr, u32 value, int size) {
    u32 p = ac3_phys(addr);
    u8 *x = ac3_ext_ptr(p, (u32)size);
    if (x) {
        for (int i = 0; i < size; i++) x[i] = (u8)(value >> (8 * i));
        return;
    }
    if (p >= 0x1F800000u && p < 0x1F800400u) {
        u8 *s = ac3_scratch + (p - 0x1F800000u);
        for (int i = 0; i < size; i++) s[i] = (u8)(value >> (8 * i));
        return;
    }
    if (p >= 0x1F801000u && p < 0x1F803000u) {
        unsigned idx = (p - 0x1F801000u) / 4;
        if (idx < sizeof(io_warned) / sizeof(io_warned[0]) && !io_warned[idx]) {
            io_warned[idx] = 1;
            fprintf(stderr, "ac3: unhandled I/O write 0x%08X = 0x%08X (%d)\n",
                    addr, value, size);
        }
        return;
    }
    fprintf(stderr, "ac3: bad write 0x%08X = 0x%08X (%d) in func 0x%08X\n",
            addr, value, size, ac3_cur_func);
}

void ac3_mult(ctx_t *ctx, u32 a, u32 b) {
    s64 r = (s64)(s32)a * (s64)(s32)b;
    ctx->lo = (u32)r;
    ctx->hi = (u32)((u64)r >> 32);
}

void ac3_multu(ctx_t *ctx, u32 a, u32 b) {
    u64 r = (u64)a * (u64)b;
    ctx->lo = (u32)r;
    ctx->hi = (u32)(r >> 32);
}

void ac3_div(ctx_t *ctx, u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) {
        ctx->lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
        ctx->hi = (u32)n;
    } else if ((u32)n == 0x80000000u && d == -1) {
        ctx->lo = 0x80000000u;
        ctx->hi = 0;
    } else {
        ctx->lo = (u32)(n / d);
        ctx->hi = (u32)(n % d);
    }
}

void ac3_divu(ctx_t *ctx, u32 a, u32 b) {
    if (b == 0) {
        ctx->lo = 0xFFFFFFFFu;
        ctx->hi = a;
    } else {
        ctx->lo = a / b;
        ctx->hi = a % b;
    }
}

ac3_func_t ac3_lookup(u32 addr) {
    unsigned lo = 0, hi = ac3_func_table_len;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ac3_func_table[mid].addr == addr) return ac3_func_table[mid].fn;
        if (ac3_func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

#define AC3_EDGE_CAP 512
typedef struct { u32 from, to; unsigned long long n; } ac3_edge_t;
static ac3_edge_t ac3_edges[AC3_EDGE_CAP];
static unsigned ac3_edge_count;

static void edge_record(u32 from, u32 to) {
    for (unsigned i = 0; i < ac3_edge_count; i++)
        if (ac3_edges[i].from == from && ac3_edges[i].to == to) {
            ac3_edges[i].n++;
            return;
        }
    if (ac3_edge_count < AC3_EDGE_CAP) {
        ac3_edges[ac3_edge_count].from = from;
        ac3_edges[ac3_edge_count].to = to;
        ac3_edges[ac3_edge_count].n = 1;
        ac3_edge_count++;
    }
}

void ac3_edges_dump(int top) {
    if (!ac3_edge_count) return;
    printf("\nindirect calls: %u distinct (caller -> target)\n", ac3_edge_count);
    for (unsigned i = 1; i < ac3_edge_count; i++) {
        ac3_edge_t k = ac3_edges[i];
        unsigned j = i;
        while (j && ac3_edges[j - 1].n < k.n) { ac3_edges[j] = ac3_edges[j - 1]; j--; }
        ac3_edges[j] = k;
    }
    for (unsigned i = 0; i < ac3_edge_count && i < (unsigned)top; i++)
        printf("  %10llu  0x%08X -> 0x%08X\n",
               ac3_edges[i].n, ac3_edges[i].from, ac3_edges[i].to);
}

void ac3_call_indirect(ctx_t *ctx, u32 target) {
    edge_record(ac3_cur_func, target);
    ac3_probe_script(ctx, target);
    ac3_func_t f = ac3_lookup(target);
    const int phase = ac3_oracle_phase_call(ctx->r[31]);
    if (phase) ac3_oracle_phase_snap(ctx, target, 30u);
    if (f) {
        if (ac3_oracle_steps_frame(target)) {
            ac3_oracle_phase_stepped(ctx, target);
        } else if (ac3_oracle_is_tick(target)) {
            ac3_oracle_tick(ctx, target, 0);
            ac3_oracle_run_tick(ctx, target, f);
            ac3_oracle_tick(ctx, target, 1);
        } else {
            f(ctx);
        }
    } else {
        ac3_interpret(ctx, target);
    }
    if (phase) ac3_oracle_phase_snap(ctx, target, 31u);
}

void ac3_jump_indirect(ctx_t *ctx, u32 target) {
    ac3_func_t f = ac3_lookup(target);
    if (f) { f(ctx); return; }
    ac3_interpret(ctx, target);
}

void ac3_unimplemented(ctx_t *ctx, u32 addr, const char *what) {
    (void)ctx;
    fprintf(stderr, "ac3: unimplemented at 0x%08X: %s\n", addr, what);
}

void ac3_prof_dump(int top) {
    extern unsigned long long ac3_prof[] __attribute__((weak));
    if (!ac3_prof) { printf("\n(profiling not compiled in)\n"); return; }

    unsigned n = ac3_func_table_len;
    unsigned *idx = (unsigned *)malloc(n * sizeof(unsigned));
    if (!idx) return;
    unsigned live = 0;
    for (unsigned i = 0; i < n; i++)
        if (ac3_prof[i]) idx[live++] = i;
    for (unsigned i = 1; i < live; i++) {
        unsigned k = idx[i], j = i;
        while (j && ac3_prof[idx[j - 1]] < ac3_prof[k]) { idx[j] = idx[j - 1]; j--; }
        idx[j] = k;
    }
    printf("\nprofile: %u of %u functions executed\n", live, n);
    for (unsigned i = 0; i < live && i < (unsigned)top; i++)
        printf("  %12llu  0x%08X\n", ac3_prof[idx[i]], ac3_func_table[idx[i]].addr);
    free(idx);
}

void ac3_trace_call(u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    fprintf(stderr, "ac3: call 0x%08X(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X)\n",
            fn, a0, a1, a2, a3);
}

u32 ac3_watch_lo, ac3_watch_hi;

#define WATCH_SLOTS 32
static struct { u32 func; u32 writes, bytes, lo, hi; } watch_by_func[WATCH_SLOTS];
static unsigned watch_slots;

#ifdef AC3_COOP_PROBE
#include "hle/coop_probe.h"
#endif
void ac3_watch_hit(u32 addr, u32 value, int size) {
#ifdef AC3_COOP_PROBE
    ac3_coop_write_hit(addr, value, size);
#endif
    ac3_probe_write_hit(addr, value, size);
    for (unsigned i = 0; i < watch_slots; i++) {
        if (watch_by_func[i].func == ac3_cur_func) {
            watch_by_func[i].writes++;
            watch_by_func[i].bytes += (u32)size;
            if (addr < watch_by_func[i].lo) watch_by_func[i].lo = addr;
            if (addr + size > watch_by_func[i].hi) watch_by_func[i].hi = addr + size;
            return;
        }
    }
    if (watch_slots < WATCH_SLOTS) {
        watch_by_func[watch_slots].func = ac3_cur_func;
        watch_by_func[watch_slots].writes = 1;
        watch_by_func[watch_slots].bytes = (u32)size;
        watch_by_func[watch_slots].lo = addr;
        watch_by_func[watch_slots].hi = addr + (u32)size;
        watch_slots++;
    }
}

void ac3_watch_bulk(u32 addr, u32 len, const char *who) {
    if (!ac3_watch_hi) return;
    if (addr + len <= ac3_watch_lo || addr >= ac3_watch_hi) return;
    printf("  watch: %-14s wrote %5u bytes at 0x%08X\n", who, len, addr);
}

void ac3_watch_dump(void) {
    if (!ac3_watch_hi) return;
    printf("\nwrites into 0x%08X..0x%08X, by writing function:\n",
           ac3_watch_lo, ac3_watch_hi);
    for (unsigned i = 0; i < watch_slots; i++)
        printf("  func 0x%08X  %6u writes  %6u bytes  range 0x%08X..0x%08X\n",
               watch_by_func[i].func, watch_by_func[i].writes,
               watch_by_func[i].bytes, watch_by_func[i].lo, watch_by_func[i].hi);
    if (!watch_slots) printf("  (nothing wrote to this range)\n");
}
