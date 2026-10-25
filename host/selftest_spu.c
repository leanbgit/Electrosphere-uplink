#include "spu_backend.h"
#include "spu_core.h"
#include "xa_adpcm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;

static void chk(const char *what, long got, long want) {
    int ok = (got == want);
    printf("  %-46s %-12ld %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected %ld\n", want); failures++; }
}

static void chk_near(const char *what, long got, long want, long tol) {
    long d = got - want;
    if (d < 0) d = -d;
    int ok = (d <= tol);
    printf("  %-46s %-12ld %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected %ld +/- %ld\n", want, tol); failures++; }
}

static void chk_true(const char *what, int cond) {
    printf("  %-46s %-12s %s\n", what, cond ? "yes" : "no",
           cond ? "ok" : "MISMATCH");
    if (!cond) failures++;
}

static void put_block(u32 addr, u8 flags, int nibble) {
    u8 *b = ac3_spu_ram + addr;
    b[0] = 0x00;
    b[1] = flags;
    for (int i = 0; i < 14; i++)
        b[2 + i] = (u8)((nibble & 0x0F) | ((nibble & 0x0F) << 4));
}

static void plain_voice(int v, u32 addr, u16 pitch) {
    spu_voice_t *a = &ac3_spu_voice[v];
    memset(a, 0, sizeof *a);
    a->start_addr = addr;
    a->pitch = pitch;
    a->vol_left = a->vol_right = 0x3FFF;
    a->volmode_left = a->volmode_right = 0;
    a->adsr1 = SPU_ADSR1_PACK(0, 0, 0xF, 0xF);
    a->adsr2 = SPU_ADSR2_PACK(0, 0, 0x7F, 0, 0);
    a->keyed_on = 1;
}

static long peak_of(const s16 *buf, int frames, int ch) {
    long p = 0;
    for (int i = 0; i < frames; i++) {
        long v = buf[i * 2 + ch];
        if (v < 0) v = -v;
        if (v > p) p = v;
    }
    return p;
}

static long extreme_of(const s16 *buf, int frames, int ch) {
    long best = 0;
    for (int i = 0; i < frames; i++) {
        long v = buf[i * 2 + ch];
        long m = v < 0 ? -v : v;
        long b = best < 0 ? -best : best;
        if (m > b) best = v;
    }
    return best;
}

#define FULL_SCALE_TOL 4

#define GAUSS_GAIN(x) ((long)(x) * 32640L / 32768L)

#define FNV_BASIS 2166136261u
static u32 fnv16(u32 h, u16 v) {
    h = (h ^ (v & 0xFFu)) * 16777619u;
    return (h ^ (u32)(v >> 8)) * 16777619u;
}

static void chk_hex(const char *what, u32 got, u32 want) {
    int ok = (got == want);
    printf("  %-46s 0x%08X   %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected 0x%08X\n", want); failures++; }
}

extern void psyq_SpuInit(ctx_t *);
extern void psyq_SpuSetReverb(ctx_t *);
extern void psyq_SpuGetReverb(ctx_t *);
extern void psyq_SpuSetReverbModeParam(ctx_t *);
extern void psyq_SpuGetReverbModeParam(ctx_t *);
extern void psyq_SpuSetReverbDepth(ctx_t *);
extern void psyq_SpuSetReverbVoice(ctx_t *);
extern void psyq_SpuClearReverbWorkArea(ctx_t *);

enum { F_END = -1, F_PARAM, F_DEPTH, F_ONOFF };
typedef struct { int fn; u32 mask, mode; s16 dl, dr; s32 delay, fb; } rcall_t;

#define G_RATTR   0x80160000u
#define G_RSTACK  0x8015F000u
#define G_FAKESPU 0x80170000u

#define L_SET_MODE_PARAM 0x8009FFC8u
#define L_SET_DEPTH      0x800A0978u
#define L_SET_REVERB     0x8009FDE8u
#define L_REG_PTR        0x800B47C4u
#define L_REV_FLAG       0x800B4758u
#define L_REV_RESERVE    0x800B475Cu
#define L_REV_OFFSET     0x800B4760u
#define L_REV_MODE       0x800B4768u
#define L_REV_DEPTH_L    0x800B476Cu
#define L_REV_DEPTH_R    0x800B476Eu
#define L_REV_DELAY      0x800B4770u
#define L_REV_FEEDBACK   0x800B4774u

static void put_rattr(const rcall_t *k) {
    MEM_W32(G_RATTR + 0, k->mask);
    MEM_W32(G_RATTR + 4, k->mode);
    MEM_W16(G_RATTR + 8, (u16)k->dl);
    MEM_W16(G_RATTR + 10, (u16)k->dr);
    MEM_W32(G_RATTR + 12, (u32)k->delay);
    MEM_W32(G_RATTR + 16, (u32)k->fb);
}

static u32 rev_call(ctx_t *c, const rcall_t *k) {
    put_rattr(k);
    c->r[4] = (k->fn == F_ONOFF) ? k->mode : G_RATTR;
    switch (k->fn) {
    case F_PARAM: psyq_SpuSetReverbModeParam(c); break;
    case F_DEPTH: psyq_SpuSetReverbDepth(c); break;
    case F_ONOFF: psyq_SpuSetReverb(c); break;
    default: break;
    }
    return c->r[2];
}

static int load_game_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u8 head[0x800];
    if (fread(head, 1, sizeof head, f) != sizeof head ||
        memcmp(head, "PS-X EXE", 8) != 0) { fclose(f); return 0; }
    u32 dest, size;
    memcpy(&dest, head + 0x18, 4);
    memcpy(&size, head + 0x1C, 4);
    u32 phys = dest & 0x1FFFFFu;
    if (phys + size > AC3_RAM_SIZE) { fclose(f); return 0; }
    int ok = fread(ac3_ram + phys, 1, size, f) == size;
    fclose(f);
    return ok;
}

typedef struct {
    u16 reg[32], base;
    s16 vol[2];
    int enable;
    u32 flag, mode, delay, fb;
    s16 depth[2];
    u32 ret[16];
    u32 ram;
} rev_snap_t;

static u32 spu_ram_hash(void) {
    u32 h = FNV_BASIS;
    for (u32 i = 0; i < SPU_RAM_SIZE; i += 2) {
        u16 v;
        memcpy(&v, ac3_spu_ram + i, 2);
        h = fnv16(h, v);
    }
    return h;
}

static void rev_start_state(void) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    c.r[29] = G_RSTACK;
    psyq_SpuInit(&c);
    for (u32 i = 0; i < SPU_RAM_SIZE; i++) ac3_spu_ram[i] = (u8)(i * 7u + 3u);
}

static void rev_hw_snap(rev_snap_t *s) {
    for (int i = 0; i < 32; i++) s->reg[i] = ac3_spu_rvb_reg(i);
    s->base = ac3_spu_rvb_base();
    s->ram = spu_ram_hash();
}

static void run_real(const rcall_t *seq, rev_snap_t *s) {
    rev_start_state();
    MEM_W32(L_REV_FLAG, 0);
    MEM_W32(L_REV_RESERVE, 0);
    MEM_W32(L_REV_OFFSET, 0xFFFE);
    MEM_W32(L_REV_MODE, 0);
    MEM_W16(L_REV_DEPTH_L, 0);
    MEM_W16(L_REV_DEPTH_R, 0);
    MEM_W32(L_REV_DELAY, 0);
    MEM_W32(L_REV_FEEDBACK, 0);
    memset(ac3_ram + (G_FAKESPU & 0x1FFFFFu), 0, 0x200);
    MEM_W16(G_FAKESPU + 0x1AA, 0xC000);
    MEM_W32(L_REG_PTR, G_FAKESPU);

    int n = 0;
    for (const rcall_t *k = seq; k->fn != F_END; k++, n++) {
        ctx_t c;
        memset(&c, 0, sizeof c);
        c.r[29] = G_RSTACK;
        put_rattr(k);
        c.r[4] = (k->fn == F_ONOFF) ? k->mode : G_RATTR;
        u32 fn = (k->fn == F_PARAM) ? L_SET_MODE_PARAM
               : (k->fn == F_DEPTH) ? L_SET_DEPTH : L_SET_REVERB;
        ac3_interp_step_budget = 1000000u;
        ac3_interpret(&c, fn);
        ac3_interp_step_budget = 0;
        s->ret[n] = c.r[2];
    }
    rev_hw_snap(s);
    s->vol[0] = (s16)MEM_R16U(G_FAKESPU + 0x184);
    s->vol[1] = (s16)MEM_R16U(G_FAKESPU + 0x186);
    s->enable = (int)((MEM_R16U(G_FAKESPU + 0x1AA) >> 7) & 1u);
    s->flag = MEM_R32(L_REV_FLAG);
    s->mode = MEM_R32(L_REV_MODE);
    s->depth[0] = (s16)MEM_R16U(L_REV_DEPTH_L);
    s->depth[1] = (s16)MEM_R16U(L_REV_DEPTH_R);
    s->delay = MEM_R32(L_REV_DELAY);
    s->fb = MEM_R32(L_REV_FEEDBACK);
}

static void run_native(const rcall_t *seq, rev_snap_t *s) {
    rev_start_state();
    ctx_t c;
    int n = 0;
    for (const rcall_t *k = seq; k->fn != F_END; k++, n++) {
        memset(&c, 0, sizeof c);
        c.r[29] = G_RSTACK;
        s->ret[n] = rev_call(&c, k);
    }
    rev_hw_snap(s);
    ac3_spu_rvb_volume(&s->vol[0], &s->vol[1]);
    s->enable = ac3_spu_rvb_enabled();
    memset(&c, 0, sizeof c);
    c.r[4] = G_RATTR;
    psyq_SpuGetReverbModeParam(&c);
    s->mode = MEM_R32(G_RATTR + 4);
    s->depth[0] = (s16)MEM_R16U(G_RATTR + 8);
    s->depth[1] = (s16)MEM_R16U(G_RATTR + 10);
    s->delay = MEM_R32(G_RATTR + 12);
    s->fb = MEM_R32(G_RATTR + 16);
    psyq_SpuGetReverb(&c);
    s->flag = c.r[2];
}

static void compare_rev_seq(const char *name, const rcall_t *seq) {
    rev_snap_t a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    run_real(seq, &a);
    run_native(seq, &b);
    const char *what = NULL;
    if (memcmp(a.reg, b.reg, sizeof a.reg) != 0)           what = "registers";
    else if (a.base != b.base)                              what = "mBASE";
    else if (a.vol[0] != b.vol[0] || a.vol[1] != b.vol[1])  what = "depth regs";
    else if (a.enable != b.enable)                          what = "enable";
    else if (a.flag != b.flag)                              what = "SpuGetReverb";
    else if (a.mode != b.mode)                              what = "mode";
    else if (a.depth[0] != b.depth[0] || a.depth[1] != b.depth[1]) what = "depth";
    else if (a.delay != b.delay || a.fb != b.fb)            what = "delay/fb";
    else if (memcmp(a.ret, b.ret, sizeof a.ret) != 0)       what = "returns";
    else if (a.ram != b.ram)                                what = "SPU RAM";
    printf("  %-46s %-12s %s\n", name, what ? what : "same", what ? "MISMATCH" : "ok");
    if (!what) return;
    failures++;
    for (int i = 0; i < 32; i++)
        if (a.reg[i] != b.reg[i])
            printf("      reg %2d: library 0x%04X, native 0x%04X\n", i, a.reg[i], b.reg[i]);
    printf("      library: mBASE 0x%04X vol %d,%d enable %d flag %u mode %u depth %d,%d delay %d fb %d\n",
           a.base, a.vol[0], a.vol[1], a.enable, a.flag, a.mode, a.depth[0], a.depth[1],
           (int)a.delay, (int)a.fb);
    printf("      native:  mBASE 0x%04X vol %d,%d enable %d flag %u mode %u depth %d,%d delay %d fb %d\n",
           b.base, b.vol[0], b.vol[1], b.enable, b.flag, b.mode, b.depth[0], b.depth[1],
           (int)b.delay, (int)b.fb);
}

int ac3_selftest_spu(const char *disc) {
    static s16 out[8192 * 2];
    failures = 0;
    printf("SPU self-test\n");

    if (!ac3_spu_ram) ac3_spu_ram = (u8 *)calloc(1, SPU_RAM_SIZE);
    ac3_spu_core_reset();

    printf("\n ADSR packing (LIBREF46.PDF p.853, p.1093 Table 15-10)\n");
    {
        u16 a1 = SPU_ADSR1_PACK(0, 0x7F, 0, 0);
        chk("adsr1 ar=0x7f alone", a1, 0x7F00);
        chk("  ar reads back", SPU_ADSR_AR(a1), 0x7F);
        chk("  dr reads back 0", SPU_ADSR_DR(a1), 0);
        chk("  sl reads back 0", SPU_ADSR_SL(a1), 0);
        a1 = SPU_ADSR1_PACK(0, 0, 0xF, 0);
        chk("adsr1 dr=0xf alone", a1, 0x00F0);
        a1 = SPU_ADSR1_PACK(0, 0, 0, 0xF);
        chk("adsr1 sl=0xf alone", a1, 0x000F);
        a1 = SPU_ADSR1_PACK(1, 0, 0, 0);
        chk("adsr1 attack mode is bit 15", a1, 0x8000);

        u16 a2 = SPU_ADSR2_PACK(0, 0, 0x7F, 0, 0);
        chk("adsr2 sr=0x7f alone (bits 12-6)", a2, 0x1FC0);
        chk("  sr reads back", SPU_ADSR_SR(a2), 0x7F);
        chk("  bit 13 stays clear", (a2 >> 13) & 1, 0);
        a2 = SPU_ADSR2_PACK(0, 0, 0, 0, 0x1F);
        chk("adsr2 rr=0x1f alone", a2, 0x001F);
        a2 = SPU_ADSR2_PACK(0, 0, 0, 1, 0);
        chk("adsr2 release mode is bit 5", a2, 0x0020);
        a2 = SPU_ADSR2_PACK(0, 1, 0, 0, 0);
        chk("adsr2 sustain direction is bit 14", a2, 0x4000);
        a2 = SPU_ADSR2_PACK(1, 0, 0, 0, 0);
        chk("adsr2 sustain mode is bit 15", a2, 0x8000);
        a1 = SPU_ADSR1_PACK(1, 0x7F, 0xF, 0xF);
        chk("adsr1 all fields set", a1, 0xFFFF);
        a2 = SPU_ADSR2_PACK(1, 1, 0x7F, 1, 0x1F);
        chk("adsr2 all fields set (bit 13 clear)", a2, 0xDFFF);
    }

    printf("\n ADPCM block decode\n");
    {
        s16 dec[28];
        s16 hist[2] = { 0, 0 };
        put_block(0x1000, 0, 1);
        u8 flags = ac3_spu_decode_block(ac3_spu_ram + 0x1000, dec, hist);
        chk("flags byte returned", flags, 0);
        chk("nibble 1, shift 0 -> 0x1000", dec[0], 0x1000);
        chk("28 samples, last one too", dec[27], 0x1000);
        hist[0] = hist[1] = 0;
        put_block(0x1000, 0, 0xF);
        ac3_spu_decode_block(ac3_spu_ram + 0x1000, dec, hist);
        chk("nibble 0xF sign-extends to -0x1000", dec[0], -0x1000);
    }

    printf("\n pitch (LIBREF46.PDF p.1092 Table 15-8)\n");
    {
        for (u32 b = 0; b < 400; b++)
            put_block(0x2000 + b * 16, (b == 399) ? 0x01 : 0x00, 1);

        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 2800);
        chk_near("pitch 0x1000: constant 0x1000 sample survives",
                 peak_of(out, 2800, 0), GAUSS_GAIN(0x1000), FULL_SCALE_TOL);

        for (u32 b = 0; b < 400; b++)
            put_block(0x2000 + b * 16, (b == 99) ? 0x01 : 0x00, 1);
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x2000);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 2800);
        chk_true("pitch 0x2000 reaches the end sooner",
                 ac3_spu_core_key_status(0) == SPU_ST_ON_ENV_OFF);
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x0800);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 2800);
        chk_true("pitch 0x0800 has not reached it yet",
                 ac3_spu_core_key_status(0) == SPU_ST_ON);
    }

    printf("\n note -> pitch (LIBREF46.PDF p.1092 Table 15-9)\n");
    chk("note == sample_note -> 0x1000",
        ac3_spu_note_to_pitch(60 << 8, 60 << 8), 0x1000);
    chk("one octave up -> 0x2000",
        ac3_spu_note_to_pitch(72 << 8, 60 << 8), 0x2000);
    chk_near("one octave down -> 0x0800",
             ac3_spu_note_to_pitch(48 << 8, 60 << 8), 0x0800, 1);
    chk_near("+7 semitones -> 0x1800 (a fifth)",
             ac3_spu_note_to_pitch(67 << 8, 60 << 8), 6137, 2);
    chk("clamped to the 0x3fff maximum",
        ac3_spu_note_to_pitch(120 << 8, 0), 0x3FFF);

    printf("\n envelope shape (LIBREF46.PDF p.1093 Table 15-11)\n");
    {
        for (u32 b = 0; b < 800; b++) put_block(0x2000 + b * 16, 0x02 | 0x01, 1);
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_voice[0].adsr1 = SPU_ADSR1_PACK(0, 0x30, 0x8, 0x7);
        ac3_spu_voice[0].adsr2 = SPU_ADSR2_PACK(0, 0, 0x7F, 0, 0x10);
        ac3_spu_core_key_on(0);
        chk("envelope starts at zero", ac3_spu_core_envx(0), 0);
        ac3_spu_core_mix(out, 16);
        s16 e1 = ac3_spu_core_envx(0);
        chk_true("attack has begun", e1 > 0);
        ac3_spu_core_mix(out, 512);
        s16 e2 = ac3_spu_core_envx(0);
        chk_true("attack is still rising", e2 > e1);
        ac3_spu_core_mix(out, 8000);
        s16 e3 = ac3_spu_core_envx(0);
        chk_true("attack reached the top and decay started", e3 <= 0x7FFF);
        ac3_spu_core_mix(out, 8000);
        s16 e4 = ac3_spu_core_envx(0);
        chk_true("decay stops just under the sustain level (SL 7 -> 0x4000)",
                 e4 < 0x4000 && e4 >= 0x4000 - 64);
        chk_true("decay fell from the peak", e4 <= e3);

        ac3_spu_voice[0].keyed_on = 0;
        ac3_spu_core_key_off(0);
        chk_true("keyed off but still sounding reports SPU_OFF_ENV_ON",
                 ac3_spu_core_key_status(0) == SPU_ST_OFF_ENV_ON);
        for (int i = 0; i < 60 && ac3_spu_core_envx(0) > 0; i++)
            ac3_spu_core_mix(out, 4096);
        chk("release reaches zero", ac3_spu_core_envx(0), 0);
        chk("and the voice then reports SPU_OFF",
            ac3_spu_core_key_status(0), SPU_ST_OFF);

        u32 fast, slow;
        for (int pass = 0; pass < 2; pass++) {
            ac3_spu_core_reset();
            plain_voice(0, 0x2000, 0x1000);
            ac3_spu_voice[0].adsr1 = SPU_ADSR1_PACK(0, pass ? 0x40 : 0x20, 0xF, 0xF);
            ac3_spu_voice[0].adsr2 = SPU_ADSR2_PACK(0, 0, 0x7F, 0, 0);
            ac3_spu_core_key_on(0);
            u32 n = 0;
            while (ac3_spu_core_envx(0) < 0x7000 && n < 4000000u) {
                ac3_spu_core_mix(out, 256);
                n += 256;
            }
            if (pass) slow = n; else fast = n;
        }
        chk_true("attack rate 0x40 is slower than 0x20", slow > fast);
    }

    printf("\n one-shot end and loop flags\n");
    {
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        put_block(0x3000, 0x04, 1);
        put_block(0x3010, 0x01, 1);
        plain_voice(0, 0x3000, 0x1000);
        ac3_spu_core_key_on(0);
        chk("just keyed on: SPU_ON_ENV_OFF, as libspu reports it",
            ac3_spu_core_key_status(0), SPU_ST_ON_ENV_OFF);
        ac3_spu_core_mix(out, 4);
        chk("a few samples in: SPU_ON", ac3_spu_core_key_status(0), SPU_ST_ON);
        ac3_spu_core_mix(out, 28 * 2 + 8);
        chk("one-shot end silences the voice", ac3_spu_core_envx(0), 0);
        chk_true("and it reports finished rather than playing",
                 ac3_spu_core_key_status(0) != SPU_ST_ON);

        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        put_block(0x3000, 0x04, 1);
        put_block(0x3010, 0x03, 1);
        plain_voice(0, 0x3000, 0x1000);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 4096);
        chk("a sustaining loop keeps playing", ac3_spu_core_key_status(0), SPU_ST_ON);
        chk_near("and keeps producing the sample",
                 peak_of(out, 4096, 0), GAUSS_GAIN(0x1000), FULL_SCALE_TOL);
    }

    printf("\n volume (LIBREF46.PDF p.1091 Table 15-7)\n");
    {
        for (u32 b = 0; b < 400; b++) put_block(0x2000 + b * 16, 0x03, 1);
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_voice[0].vol_left = 0x3FFF;
        ac3_spu_voice[0].vol_right = 0;
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 1024);
        chk_near("left at 0x3fff is (very nearly) unity",
                 peak_of(out, 1024, 0), GAUSS_GAIN(0x1000), FULL_SCALE_TOL);
        chk("right at 0 is silent", peak_of(out, 1024, 1), 0);

        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_voice[0].vol_left = 0x2000;
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 1024);
        chk_near("half volume halves the output",
                 peak_of(out, 1024, 0), GAUSS_GAIN(0x0800), FULL_SCALE_TOL);

        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_voice[0].vol_left = -0x3FFF;
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 1024);
        chk_true("negative volume inverts the phase",
                 extreme_of(out, 1024, 0) < 0);

        ac3_spu_core_reset();
        ac3_spu_set_master(0x2000, 0x2000, 0, 0);
        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 1024);
        chk_near("master at half scales the mix",
                 peak_of(out, 1024, 0), GAUSS_GAIN(0x0800), FULL_SCALE_TOL);
        ac3_spu_set_mute(1);
        ac3_spu_core_mix(out, 1024);
        chk("SpuSetMute silences the output", peak_of(out, 1024, 0), 0);
        ac3_spu_set_mute(0);
    }

    printf("\n CD input (LIBREF46.PDF p.1058)\n");
    {
        ac3_spu_core_reset();
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        static s16 tone[512 * 2];
        for (int i = 0; i < 512; i++) {
            tone[i * 2 + 0] = 0x4000;
            tone[i * 2 + 1] = -0x4000;
        }
        ac3_spu_set_cd_volume(0x7FFF, 0x7FFF);
        ac3_spu_set_cd_mix(1);
        ac3_spu_cd_push(tone, 512);
        chk("512 frames queued", ac3_spu_cd_queued(), 512);
        ac3_spu_core_mix(out, 512);
        chk("queue drained by the mix", ac3_spu_cd_queued(), 0);
        chk_near("CD at full volume passes through", peak_of(out, 512, 0), 0x4000, 4);
        chk_true("and the right channel keeps its inverted phase", out[1] < 0);

        ac3_spu_set_cd_mix(0);
        ac3_spu_cd_push(tone, 512);
        ac3_spu_core_mix(out, 512);
        chk("cd.mix off silences the CD input", peak_of(out, 512, 0), 0);
        ac3_spu_set_cd_mix(1);

        ac3_spu_set_cd_volume(0x4000, 0x4000);
        ac3_spu_cd_push(tone, 512);
        ac3_spu_core_mix(out, 512);
        chk_near("half CD volume halves it", peak_of(out, 512, 0), 0x2000, 4);

        const s16 *cdl = ac3_spu_decoded(0);
        const s16 *cdr = ac3_spu_decoded(1);
        chk_true("cd_left tap is at SPU RAM 0x000",
                 (const u8 *)cdl == ac3_spu_ram);
        chk_true("cd_right tap is at SPU RAM 0x400",
                 (const u8 *)cdr == ac3_spu_ram + 0x400);
        chk("cd_left tap holds the undamped input", cdl[0], 0x4000);
    }

    printf("\n reverb work areas (LIBREF46.PDF p.1080 Table 15-5)\n");
    chk("SPU_REV_MODE_ROOM     0x26c0", ac3_spu_reverb_work_size(SPU_REV_MODE_ROOM), 0x26C0);
    chk("SPU_REV_MODE_STUDIO_A 0x1f40", ac3_spu_reverb_work_size(SPU_REV_MODE_STUDIO_A), 0x1F40);
    chk("SPU_REV_MODE_STUDIO_B 0x4840", ac3_spu_reverb_work_size(SPU_REV_MODE_STUDIO_B), 0x4840);
    chk("SPU_REV_MODE_STUDIO_C 0x6fe0", ac3_spu_reverb_work_size(SPU_REV_MODE_STUDIO_C), 0x6FE0);
    chk("SPU_REV_MODE_HALL     0xade0", ac3_spu_reverb_work_size(SPU_REV_MODE_HALL), 0xADE0);
    chk("SPU_REV_MODE_SPACE    0xf6c0", ac3_spu_reverb_work_size(SPU_REV_MODE_SPACE), 0xF6C0);
    chk("SPU_REV_MODE_ECHO     0x18040", ac3_spu_reverb_work_size(SPU_REV_MODE_ECHO), 0x18040);
    chk("SPU_REV_MODE_DELAY    0x18040", ac3_spu_reverb_work_size(SPU_REV_MODE_DELAY), 0x18040);
    chk("SPU_REV_MODE_PIPE     0x3c00", ac3_spu_reverb_work_size(SPU_REV_MODE_PIPE), 0x3C00);
    chk("SPU_REV_MODE_OFF      0x80", ac3_spu_reverb_work_size(SPU_REV_MODE_OFF), 0x80);

    printf("\n reverb unit (against Beetle PSX's output for the same input)\n");
    {
        static const struct { const char *name; int mode; u32 out, area; } fp[] = {
            { "studio B (AC3's mode)", SPU_REV_MODE_STUDIO_B, 0xD822E8A3u, 0x1C71360Bu },
            { "hall",                  SPU_REV_MODE_HALL,     0x1A753DCAu, 0xC5B4855Au },
        };
        for (unsigned i = 0; i < sizeof fp / sizeof fp[0]; i++) {
            ac3_spu_core_reset();
            memset(ac3_spu_ram, 0, SPU_RAM_SIZE);
            const u16 *preset = ac3_spu_reverb_preset(fp[i].mode);
            for (int r = 0; r < 32; r++) ac3_spu_rvb_set_reg(r, preset[r]);
            u32 base = ac3_spu_reverb_work_start(fp[i].mode) / 8u;
            ac3_spu_rvb_set_base((u16)base);
            ac3_spu_rvb_set_enable(1);
            u32 h = FNV_BASIS;
            for (u32 t = 0; t < 88200; t++) {
                s32 x = (t % 4410 < 64) ? (s32)(((t * 2654435761u) >> 16) & 0x7FFF) - 0x4000 : 0;
                s32 in[2] = { x, -x / 2 }, o[2];
                ac3_spu_rvb_run(in, o);
                h = fnv16(h, (u16)o[0]);
                h = fnv16(h, (u16)o[1]);
            }
            u32 ha = FNV_BASIS;
            for (u32 w = base << 2; w < SPU_RAM_SIZE / 2u; w++) {
                u16 v;
                memcpy(&v, ac3_spu_ram + w * 2u, 2);
                ha = fnv16(ha, v);
            }
            char what[64];
            snprintf(what, sizeof what, "%s: output", fp[i].name);
            chk_hex(what, h, fp[i].out);
            snprintf(what, sizeof what, "%s: work area", fp[i].name);
            chk_hex(what, ha, fp[i].area);
        }
    }

    printf("\n reverb in the mix (AC3's own settings)\n");
    {
        ctx_t c;
        memset(&c, 0, sizeof c);
        for (u32 b = 0; b < 200; b++) put_block(0x2000 + b * 16, 0x03, 1);
        psyq_SpuInit(&c);
        ac3_spu_set_master(0x3FFF, 0x3FFF, 0, 0);
        rev_call(&c, &(rcall_t){ F_PARAM, 0x01, SPU_REV_MODE_STUDIO_B, 0, 0, 0, 0 });
        c.r[4] = SPU_REV_MODE_STUDIO_B;
        psyq_SpuClearReverbWorkArea(&c);
        rev_call(&c, &(rcall_t){ F_DEPTH, 0x06, 0, 0x1999, 0x1999, 0, 0 });
        rev_call(&c, &(rcall_t){ F_ONOFF, 0, SPU_LIB_ON, 0, 0, 0, 0 });
        c.r[4] = SPU_LIB_ON; c.r[5] = 0xFFFFFF;
        psyq_SpuSetReverbVoice(&c);
        chk("mBASE is STUDIO_B's start", ac3_spu_rvb_base(), 0xF6F8);

        u32 wstart = ac3_spu_reverb_work_start(SPU_REV_MODE_STUDIO_B);
        memset(ac3_spu_ram + 0x10000, 0x5A, wstart - 0x10000);

        plain_voice(0, 0x2000, 0x1000);
        ac3_spu_core_key_on(0);
        ac3_spu_core_mix(out, 8000);
        ac3_spu_voice[0].keyed_on = 0;
        ac3_spu_core_key_off(0);
        while (ac3_spu_core_envx(0) > 0) ac3_spu_core_mix(out, 64);
        ac3_spu_core_mix(out, 4096);
        long tail = peak_of(out, 4096, 0);
        chk_true("the reverb keeps sounding after the voice stops", tail > 0);
        for (int i = 0; i < 40; i++) ac3_spu_core_mix(out, 4096);
        chk_true("and dies away", peak_of(out, 4096, 0) < tail);

        int clean = 1;
        for (u32 a = 0x10000; a < wstart; a++)
            if (ac3_spu_ram[a] != 0x5A) { clean = 0; break; }
        chk_true("its writes stay inside its work area", clean);

        rev_call(&c, &(rcall_t){ F_ONOFF, 0, SPU_LIB_OFF, 0, 0, 0, 0 });
        static u8 area[0x4840];
        memcpy(area, ac3_spu_ram + wstart, sizeof area);
        ac3_spu_core_mix(out, 4096);
        chk_true("reverb off: the work area is not written",
                 memcmp(area, ac3_spu_ram + wstart, sizeof area) == 0);
        rev_call(&c, &(rcall_t){ F_DEPTH, 0x06, 0, 0, 0, 0, 0 });
        ac3_spu_core_mix(out, 4096);
        chk("depth 0 takes it out of the mix", peak_of(out, 4096, 0), 0);
        c.r[4] = SPU_LIB_OFF; c.r[5] = 0xFFFFFF;
        psyq_SpuSetReverbVoice(&c);
    }

    {
        ctx_t c;
        memset(&c, 0, sizeof c);
        memset(ac3_spu_ram, 0x77, SPU_RAM_SIZE);
        c.r[4] = SPU_REV_MODE_STUDIO_B;
        psyq_SpuClearReverbWorkArea(&c);
        chk("SpuClearReverbWorkArea(STUDIO_B) returns 0", (long)c.r[2], 0);
        chk_true("  and clears 0x7B7C0 up", ac3_spu_ram[0x7B7C0] == 0 &&
                 ac3_spu_ram[SPU_RAM_SIZE - 1] == 0);
        chk_true("  but not below it", ac3_spu_ram[0x7B7BF] == 0x77);
        memset(ac3_spu_ram, 0x77, SPU_RAM_SIZE);
        c.r[4] = SPU_REV_MODE_OFF;
        psyq_SpuClearReverbWorkArea(&c);
        chk_true("mode OFF clears the last 0x80 bytes only",
                 ac3_spu_ram[0x7FF80] == 0 && ac3_spu_ram[0x7FF7F] == 0x77);
        c.r[4] = SPU_REV_MODE_MAX;
        psyq_SpuClearReverbWorkArea(&c);
        chk("a bad mode is refused", (s32)c.r[2], SPU_ERROR);
        memset(ac3_spu_ram, 0, SPU_RAM_SIZE);
    }

    printf("\n libspu reverb against the library's own code\n");
    if (!load_game_exe("game/ACE3.BIN")) {
        printf("  game/ACE3.BIN not found -- skipped\n");
    } else {
        static const rcall_t ac3_seq[] = {
            { F_PARAM, 0x01, SPU_REV_MODE_STUDIO_B, 0, 0, 0, 0 },
            { F_DEPTH, 0x06, 0, 0x1999, 0x1999, 0, 0 },
            { F_ONOFF, 0, SPU_LIB_ON, 0, 0, 0, 0 },
            { F_PARAM, 0x01, SPU_REV_MODE_STUDIO_B, 0, 0, 0, 0 },
            { F_DEPTH, 0x06, 0, 0, 0, 0, 0 },
            { F_ONOFF, 0, SPU_LIB_OFF, 0, 0, 0, 0 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const rcall_t echo_seq[] = {
            { F_PARAM, 0x01, SPU_REV_MODE_ECHO, 0, 0, 0, 0 },
            { F_PARAM, 0x08, 0, 0, 0, 40, 0 },
            { F_PARAM, 0x10, 0, 0, 0, 0, 90 },
            { F_PARAM, 0x18, 0, 0, 0, 100, 10 },
            { F_PARAM, 0x02, 0, 0x2000, 0x1234, 0, 0 },
            { F_PARAM, 0x08, 0, 0, 0, -5, 0 },
            { F_PARAM, 0x10, 0, 0, 0, 0, 200 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const rcall_t delay_seq[] = {
            { F_PARAM, 0x00, SPU_REV_MODE_DELAY, 0x100, 0x200, 64, 127 },
            { F_PARAM, 0x08, 0, 0, 0, 127, 0 },
            { F_PARAM, 0x1E, 0, -0x4000, 0x4000, 1, 1 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const rcall_t hall_seq[] = {
            { F_PARAM, 0x01, SPU_REV_MODE_HALL, 0, 0, 0, 0 },
            { F_PARAM, 0x18, 0, 0, 0, 50, 50 },
            { F_PARAM, 0x06, 0, 0x7FFF, -0x8000, 0, 0 },
            { F_DEPTH, 0x00, 0, -1, -1, 0, 0 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const rcall_t bad_seq[] = {
            { F_PARAM, 0x01, SPU_REV_MODE_ROOM, 0, 0, 0, 0 },
            { F_PARAM, 0x01, 10, 0, 0, 0, 0 },
            { F_PARAM, 0x01, 0x10A, 0, 0, 0, 0 },
            { F_PARAM, 0x00, 0xFFFFFFFFu, 0, 0, 0, 0 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const rcall_t onoff_seq[] = {
            { F_ONOFF, 0, SPU_LIB_ON, 0, 0, 0, 0 },
            { F_PARAM, 0x01, SPU_REV_MODE_STUDIO_A, 0, 0, 0, 0 },
            { F_ONOFF, 0, 2, 0, 0, 0, 0 },
            { F_ONOFF, 0, SPU_LIB_OFF, 0, 0, 0, 0 },
            { F_PARAM, 0x01, SPU_REV_MODE_SPACE, 0, 0, 0, 0 },
            { F_END, 0, 0, 0, 0, 0, 0 },
        };
        static const struct { const char *name; const rcall_t *seq; } seqs[] = {
            { "AC3's own sequence",          ac3_seq },
            { "ECHO: delay and feedback",    echo_seq },
            { "DELAY: every field at once",  delay_seq },
            { "HALL ignores delay/feedback", hall_seq },
            { "bad modes are refused",       bad_seq },
            { "enable across mode changes",  onoff_seq },
        };
        for (unsigned i = 0; i < sizeof seqs / sizeof seqs[0]; i++)
            compare_rev_seq(seqs[i].name, seqs[i].seq);

        for (u32 m = 0; m < SPU_REV_MODE_MAX; m++) {
            rcall_t seq[2] = {
                { F_PARAM, 0x00, m | SPU_REV_MODE_CLEAR_WA, 0x3000, -0x2000, 33, 99 },
                { F_END, 0, 0, 0, 0, 0, 0 },
            };
            char name[64];
            snprintf(name, sizeof name, "mode %u, all fields", m);
            compare_rev_seq(name, seq);
        }
        memset(ac3_ram, 0, AC3_RAM_SIZE);
    }

    printf("\n key status (psyq/include/LIBSPU.H:30-58)\n");
    {
        ac3_spu_core_reset();
        chk("an untouched voice is SPU_OFF", ac3_spu_core_key_status(5), SPU_ST_OFF);
        for (u32 b = 0; b < 200; b++) put_block(0x2000 + b * 16, 0x03, 1);
        plain_voice(5, 0x2000, 0x1000);
        ac3_spu_core_key_on(5);
        ac3_spu_core_mix(out, 64);
        chk("keyed on and sounding is SPU_ON", ac3_spu_core_key_status(5), SPU_ST_ON);
        ac3_spu_voice[5].keyed_on = 0;
        ac3_spu_core_key_off(5);
        chk("keyed off, still in release, is SPU_OFF_ENV_ON",
            ac3_spu_core_key_status(5), SPU_ST_OFF_ENV_ON);
    }

    printf("\n a faded loop frees its voice (LIBSPU.LIB SR_GAKS)\n");
    {
        for (u32 b = 0; b < 64; b++)
            put_block(0x2000 + b * 16, (b == 0 ? 0x04 : 0) | (b == 63 ? 0x03 : 0), 1);
        ac3_spu_core_reset();
        plain_voice(7, 0x2000, 0x1000);
        ac3_spu_voice[7].adsr2 = SPU_ADSR2_PACK(0, 1, 0x20, 0, 0);
        ac3_spu_core_key_on(7);
        for (int f = 0; f < 60 && ac3_spu_core_envx(7) != 0; f++)
            ac3_spu_core_mix(out, 735);
        chk("the loop has faded to ENVX 0", ac3_spu_core_envx(7), 0);
        chk("keyed on at ENVX 0 is SPU_ON_ENV_OFF, not SPU_ON",
            ac3_spu_core_key_status(7), SPU_ST_ON_ENV_OFF);
        ac3_spu_voice[7].keyed_on = 0;
        ac3_spu_core_key_off(7);
        ac3_spu_core_mix(out, 64);
        chk("and once the driver keys it off, SPU_OFF", ac3_spu_core_key_status(7),
            SPU_ST_OFF);
    }

    printf("\n note -> pitch against _spu_note2pitch (LIBSPU.LIB S_N2P)\n");
    chk("sample fine +64 raises the pitch (60 vs 60+64/128)",
        ac3_spu_note_to_pitch(60 << 8, (60 << 8) | 64), 4216);
    chk("two octaves up is pinned at 0x3fff",
        ac3_spu_note_to_pitch(84 << 8, 60 << 8), 0x3FFF);
    chk("23 semitones up, sample fine 127",
        ac3_spu_note_to_pitch(47 << 8, (24 << 8) | 127), 16376);
    chk("an octave and a fifth down, fine 37 on the note",
        ac3_spu_note_to_pitch((41 << 8) | 37, 60 << 8), 1390);

    printf("\n ADPCM decode corners\n");
    {
        s16 dec[28];
        s16 hist[2] = { -1, 0 };
        u8 *b = ac3_spu_ram + 0x1000;
        memset(b, 0, 16);
        b[0] = 0x10;
        ac3_spu_decode_block(b, dec, hist);
        chk("filter 1 on history -1 rounds down to -1", dec[0], -1);
        hist[0] = 0x4000; hist[1] = 0x4000;
        b[0] = 0x50;
        b[2] = 0x01;
        ac3_spu_decode_block(b, dec, hist);
        chk("filter 5 predicts nothing", dec[0], 0x1000);
        hist[0] = hist[1] = 0;
        b[0] = 0x0D;
        b[2] = 0x91;
        ac3_spu_decode_block(b, dec, hist);
        chk("shift 13, nibble 1: sign bit clear -> 0", dec[0], 0);
        chk("shift 13, nibble 9: 0x8000 >> 8", dec[1], -128);
    }

    printf("\n repeat-address register\n");
    {
        ac3_spu_core_reset();
        put_block(0x4000, 0x04, 1);
        put_block(0x4010, 0x03, 1);
        put_block(0x5000, 0x03, 2);
        plain_voice(2, 0x4000, 0x1000);
        ac3_spu_core_set_loop_addr(2, 0x5000);
        ac3_spu_core_key_on(2);
        ac3_spu_core_mix(out, 28 * 3);
        chk("before key-on: the loop-start flag takes over",
            (long)ac3_spu_core_loop_addr(2), 0x4000);
        ac3_spu_core_set_loop_addr(2, 0x5000);
        ac3_spu_core_mix(out, 28 * 4);
        chk("while playing: the written address holds",
            (long)ac3_spu_core_loop_addr(2), 0x5000);
    }

    printf("\n sound-buffer heap (LIBREF46.PDF p.1048-1055, p.1075)\n");
    {
        extern void psyq_SpuInitMalloc(ctx_t *);
        extern void psyq_SpuMalloc(ctx_t *);
        extern void psyq_SpuMallocWithStartAddr(ctx_t *);
        extern void psyq_SpuFree(ctx_t *);
        extern void psyq_SpuReserveReverbWorkArea(ctx_t *);
        extern void psyq_SpuIsReverbWorkAreaReserved(ctx_t *);

        ctx_t c;
        memset(&c, 0, sizeof c);
        ac3_spu_core_reset();

        c.r[4] = 16; c.r[5] = 0x80000000u;
        psyq_SpuInitMalloc(&c);
        chk("SpuInitMalloc returns the block count", (long)c.r[2], 16);

        c.r[4] = 0x1000;
        psyq_SpuMalloc(&c);
        chk("the first block starts at 0x01010", (long)c.r[2], 0x1010);
        u32 first = c.r[2];
        c.r[4] = 0x1000;
        psyq_SpuMalloc(&c);
        chk_true("the second block does not overlap it",
                 c.r[2] >= first + 0x1000);
        u32 second = c.r[2];

        c.r[4] = 0x40000; c.r[5] = 0x100;
        psyq_SpuMallocWithStartAddr(&c);
        chk("WithStartAddr honours the address asked for", (long)c.r[2], 0x40000);

        c.r[4] = SPU_RAM_SIZE;
        psyq_SpuMalloc(&c);
        chk("an oversized request returns -1", (s32)c.r[2], -1);

        c.r[4] = first;
        psyq_SpuFree(&c);
        c.r[4] = 0x1000;
        psyq_SpuMalloc(&c);
        chk("a freed block is handed out again", (long)c.r[2], (long)first);
        c.r[4] = second;
        psyq_SpuFree(&c);

        rev_call(&c, &(rcall_t){ F_PARAM, 0x01, SPU_REV_MODE_HALL, 0, 0, 0, 0 });
        c.r[4] = SPU_LIB_ON;
        psyq_SpuReserveReverbWorkArea(&c);
        chk("reserving a free work area succeeds", (long)c.r[2], SPU_LIB_ON);
        c.r[4] = SPU_CHECK;
        psyq_SpuIsReverbWorkAreaReserved(&c);
        chk("and it now reports reserved", (long)c.r[2], SPU_LIB_ON);

        c.r[4] = SPU_RAM_SIZE - 0xADE0; c.r[5] = 0x100;
        psyq_SpuMallocWithStartAddr(&c);
        chk_true("SpuMalloc cannot allocate inside a reserved work area",
                 (s32)c.r[2] == -1 || c.r[2] < SPU_RAM_SIZE - 0xADE0);

        c.r[4] = SPU_LIB_OFF;
        psyq_SpuReserveReverbWorkArea(&c);
        c.r[4] = SPU_RAM_SIZE - 0x100; c.r[5] = 0x100;
        psyq_SpuMallocWithStartAddr(&c);
        chk("the top of the buffer can be allocated once released",
            (long)c.r[2], (long)(SPU_RAM_SIZE - 0x100));
        c.r[4] = SPU_DIAG;
        psyq_SpuIsReverbWorkAreaReserved(&c);
        chk("SPU_DIAG now says the area cannot be reserved",
            (long)c.r[2], SPU_LIB_OFF);
        c.r[4] = SPU_LIB_ON;
        psyq_SpuSetReverb(&c);
        chk("SpuSetReverb reports SPU_OFF rather than pretending",
            (long)c.r[2], SPU_LIB_OFF);
        chk("and reverb really is off", ac3_spu_rvb_enabled(), 0);

        c.r[4] = SPU_RAM_SIZE - 0x100;
        psyq_SpuFree(&c);
        c.r[4] = SPU_LIB_ON;
        psyq_SpuSetReverb(&c);
        chk("with the area free, reverb turns on", (long)c.r[2], SPU_LIB_ON);
        rev_call(&c, &(rcall_t){ F_ONOFF, 0, SPU_LIB_OFF, 0, 0, 0, 0 });
        c.r[4] = 0; c.r[5] = 0;
        psyq_SpuInitMalloc(&c);
    }

    printf("\n CD-XA decode (Filefrmt.pdf p.18 Table 1-1)\n");
    {
        static u8 sec[2340];
        ac3_spu_core_reset();
        ac3_xa_reset();
        ac3_xa_set_enabled(1);

        memset(sec, 0, sizeof sec);
        sec[6] = 0x04;
        sec[7] = 0x01;
        for (int g = 0; g < 18; g++) {
            u8 *blk = sec + 12 + g * 128;
            for (int i = 0; i < 16; i++) blk[i] = 0x08;
            for (int i = 0; i < 112; i++) blk[16 + i] = 0x11;
        }
        chk_true("an audio sector is accepted", ac3_xa_sector(sec) == 1);
        chk("one sector decoded", ac3_xa_sectors_played(), 1);
        chk("37.8kHz stereo -> 2352 frames at 44.1kHz",
            ac3_spu_cd_queued(), 2352);

        ac3_xa_reset();
        ac3_xa_set_enabled(1);
        sec[7] = 0x04;
        ac3_xa_sector(sec);
        chk("18.9kHz mono -> 9408 frames at 44.1kHz",
            ac3_spu_cd_queued(), 9408);

        ac3_xa_reset();
        ac3_xa_set_enabled(1);
        sec[6] = 0x08;
        chk("a data sector is not audio", ac3_xa_sector(sec), 0);

        ac3_xa_reset();
        sec[6] = 0x04;
        ac3_xa_set_enabled(0);
        chk("with CdlModeRT clear the sector is still consumed",
            ac3_xa_sector(sec), 1);
        chk("but no audio is produced", ac3_spu_cd_queued(), 0);

        ac3_xa_reset();
        ac3_xa_set_enabled(1);
        sec[4] = 1; sec[5] = 7;
        ac3_xa_set_filter(1, 1, 9);
        ac3_xa_sector(sec);
        chk("a sector on another channel is filtered out",
            ac3_spu_cd_queued(), 0);
        ac3_xa_set_filter(1, 1, 7);
        ac3_xa_sector(sec);
        chk_true("the selected channel plays", ac3_spu_cd_queued() > 0);

        ac3_xa_reset();
        ac3_xa_set_enabled(1);
        sec[7] = 0x01;
        ac3_xa_set_muted(1);
        ac3_xa_sector(sec);
        chk("CdlMute produces silence, not nothing", ac3_spu_cd_queued(), 2352);
        chk("and the samples really are silent", ac3_xa_peak(), 0);
        ac3_xa_set_muted(0);
    }

    if (disc && *disc) {
        printf("\n CD-XA against %s\n", disc);
        FILE *f = fopen(disc, "rb");
        if (!f) {
            printf("  cannot open the image -- skipped\n");
        } else {
            ac3_spu_core_reset();
            ac3_xa_reset();
            ac3_xa_set_enabled(1);
            long blocks = 0, redundant = 0, audio = 0;
            u32 first_lba = 0;
            u8 raw[2352];
            for (long lba = 0; lba < 400000 && audio < 40; lba++) {
                if (fseek(f, lba * 2352L, SEEK_SET) != 0) break;
                if (fread(raw, 1, sizeof raw, f) != sizeof raw) break;
                const u8 *sect = raw + 12;
                if (!(sect[6] & 0x04) || (sect[6] & 0x08)) continue;
                if (sect[5] == 0xFF) continue;
                if (!audio) {
                    first_lba = (u32)lba;
                    ac3_xa_set_filter(1, sect[4], sect[5]);
                }
                for (int g = 0; g < 18; g++) {
                    const u8 *p = sect + 12 + g * 128;
                    blocks++;
                    if (!memcmp(p, p + 4, 4) && !memcmp(p + 8, p + 12, 4))
                        redundant++;
                }
                ac3_xa_sector(sect);
                audio++;
            }
            fclose(f);
            printf("  first audio sector at lba %u, %ld sectors read\n",
                   first_lba, audio);
            chk_true("the disc has XA audio sectors", audio > 0);
            if (blocks) {
                chk("every sound-parameter header is redundant as expected",
                    redundant, blocks);
                chk_true("and decoding produced samples", ac3_xa_peak() > 0);
                chk_true("the decoded audio is not silent", ac3_xa_peak() > 100);
                chk_true("and does not clip", ac3_xa_peak() < 32767);
            }
        }
    } else {
        printf("\n CD-XA against a real disc: no image given -- skipped\n");
        printf("   (pass one: ac3 --selftest-spu <disc.bin>)\n");
    }

    printf("\n%s (%d failures)\n",
           failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    printf("note: this checks the mixer against what the SDK documents -- the\n"
           "      ADSR field widths and curve shapes, the pitch and note\n"
           "      scales, the volume and CD ranges, the reverb work-area sizes\n"
           "      and the XA formats -- and against libspu's own code where the\n"
           "      SDK is silent (key status, note to pitch, the reverb presets\n"
           "      and how delay, feedback and depth load them). The envelope\n"
           "      timing, ADPCM decode, Gaussian interpolation and the reverb\n"
           "      unit follow the hardware as the reference emulators model it;\n"
           "      the reverb is checked against Beetle PSX's own output.\n");
    return failures ? 1 : 0;
}
