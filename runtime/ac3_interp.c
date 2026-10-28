#include "ac3_runtime.h"
#include "hle/ac3_stream_probe.h"
#include <stdio.h>
#include <stdlib.h>

#define OP(w)    ((w) >> 26)
#define RS(w)    (((w) >> 21) & 0x1F)
#define RT(w)    (((w) >> 16) & 0x1F)
#define RD(w)    (((w) >> 11) & 0x1F)
#define SHAMT(w) (((w) >> 6) & 0x1F)
#define FUNCT(w) ((w) & 0x3F)
#define IMM(w)   ((w) & 0xFFFF)
#define SIMM(w)  ((u32)(s32)(s16)((w) & 0xFFFF))
#define TARG(w)  ((w) & 0x03FFFFFF)

#define SET(n, v) do { u32 _n = (n); if (_n) ctx->r[_n] = (u32)(v); } while (0)

#define INTERP_SENTINEL 0xDEAD0000u

/* Overlays run as short calls, so a runaway loop is cut off after 200M steps.
 * In the interpreter-only build the whole game runs here: no limit. */
#ifdef AC3_INTERP_ONLY
#define MAX_STEPS 0xFFFFFFFFFFFFFFFFull
#else
#define MAX_STEPS 200000000ull
#endif

extern void ac3_bios_call(ctx_t *ctx, u32 vector, u32 funcnum);
extern void psyq_setjmp(ctx_t *ctx);
extern void ac3_syscall(ctx_t *ctx);
extern void ac3_break(ctx_t *ctx);

void ac3_interpret(ctx_t *ctx, u32 entry);

extern u32 ac3_vblank_count(void);
extern u32 ac3_interp_pc;
static void ovl_trace(u32 caller, u32 target) {
    static int lo = -1, hi;
    if (lo < 0) {
        const char *s = getenv("AC3_OVL_TRACE");
        lo = 0; hi = -1;
        if (s && *s) {
            char *end;
            lo = (int)strtol(s, &end, 0);
            hi = (*end == '-') ? (int)strtol(end + 1, NULL, 0) : lo;
        }
    }
    if (hi < 0) return;
    int f = (int)ac3_vblank_count();
    if (f < lo || f > hi) return;
    fprintf(stderr, "  [ovl f%5d] 0x%08X -> 0x%08X\n", f, caller, target);
}

extern int ac3_disc_scene_query(ctx_t *ctx, u32 target);

/* The interpreter runs each called function with r31 set to a sentinel, so
 * code that asks "who called me?" (the widescreen view widening in gte.c)
 * reads the guest's real return address from here instead. */
u32 ac3_guest_ra;

#ifdef AC3_COOP_PROBE
#include "hle/coop_probe.h"
#define COOP_READ(addr, n) do { u32 _a = (addr); \
    if (ac3_coop_r_hi && _a >= ac3_coop_r_lo && _a < ac3_coop_r_hi) ac3_coop_read_hit(_a, (n)); } while (0)
#else
#define COOP_READ(addr, n) do { } while (0)
#endif

static void call_at(ctx_t *ctx, u32 target) {
#ifdef AC3_COOP_PROBE
    ac3_coop_call(target);
    ac3_coop_call_args(ctx, target);
#endif
    ovl_trace(ac3_interp_pc, target);
    ac3_probe_overlay_call(ctx, target);
    if (ac3_disc_scene_query(ctx, target)) return;
    ac3_func_t f = ac3_lookup(target);
    if (f) {
        f(ctx);
        return;
    }
    ac3_interpret(ctx, target);
}

typedef struct { u32 target; int taken; int is_jump; u32 word; } xfer_t;

static void exec_one(ctx_t *ctx, u32 w, u32 pc, xfer_t *x) {
    x->taken = 0;
    x->is_jump = 0;
    x->target = 0;
    x->word = w;
    {
        int taken = 0, is_jump = 0;
        u32 branch_to = 0;

        switch (OP(w)) {
        case 0x00:
            switch (FUNCT(w)) {
            case 0x00: SET(RD(w), SHL(ctx->r[RT(w)], SHAMT(w))); break;
            case 0x02: SET(RD(w), SHR(ctx->r[RT(w)], SHAMT(w))); break;
            case 0x03: SET(RD(w), SAR(ctx->r[RT(w)], SHAMT(w))); break;
            case 0x04: SET(RD(w), SHL(ctx->r[RT(w)], ctx->r[RS(w)])); break;
            case 0x06: SET(RD(w), SHR(ctx->r[RT(w)], ctx->r[RS(w)])); break;
            case 0x07: SET(RD(w), SAR(ctx->r[RT(w)], ctx->r[RS(w)])); break;
            case 0x08:
                branch_to = ctx->r[RS(w)]; is_jump = 1; taken = 1; break;
            case 0x09:
                branch_to = ctx->r[RS(w)]; is_jump = 1; taken = 1;
                SET(RD(w), pc + 8);
                break;
            case 0x0C: ac3_syscall(ctx); break;
            case 0x0D: ac3_break(ctx); break;
            case 0x10: SET(RD(w), ctx->hi); break;
            case 0x11: ctx->hi = ctx->r[RS(w)]; break;
            case 0x12: SET(RD(w), ctx->lo); break;
            case 0x13: ctx->lo = ctx->r[RS(w)]; break;
            case 0x18: ac3_mult(ctx, ctx->r[RS(w)], ctx->r[RT(w)]); break;
            case 0x19: ac3_multu(ctx, ctx->r[RS(w)], ctx->r[RT(w)]); break;
            case 0x1A: ac3_div(ctx, ctx->r[RS(w)], ctx->r[RT(w)]); break;
            case 0x1B: ac3_divu(ctx, ctx->r[RS(w)], ctx->r[RT(w)]); break;
            case 0x20: case 0x21:
                SET(RD(w), ctx->r[RS(w)] + ctx->r[RT(w)]); break;
            case 0x22: case 0x23:
                SET(RD(w), ctx->r[RS(w)] - ctx->r[RT(w)]); break;
            case 0x24: SET(RD(w), ctx->r[RS(w)] & ctx->r[RT(w)]); break;
            case 0x25: SET(RD(w), ctx->r[RS(w)] | ctx->r[RT(w)]); break;
            case 0x26: SET(RD(w), ctx->r[RS(w)] ^ ctx->r[RT(w)]); break;
            case 0x27: SET(RD(w), ~(ctx->r[RS(w)] | ctx->r[RT(w)])); break;
            case 0x2A: SET(RD(w), SLT(ctx->r[RS(w)], ctx->r[RT(w)])); break;
            case 0x2B: SET(RD(w), SLTU(ctx->r[RS(w)], ctx->r[RT(w)])); break;
            default:
                ac3_probe_unknown_insn(pc, w);
                break;
            }
            break;

        case 0x01: {
            s32 v = (s32)ctx->r[RS(w)];
            u32 kind = RT(w);
            int cond = (kind & 1) ? (v >= 0) : (v < 0);
            if (kind & 0x10) ctx->r[31] = pc + 8;
            branch_to = pc + 4 + (SIMM(w) << 2);
            taken = cond;
            break;
        }

        case 0x02: branch_to = ((pc + 4) & 0xF0000000u) | (TARG(w) << 2);
                   is_jump = 1; taken = 1; break;
        case 0x03: branch_to = ((pc + 4) & 0xF0000000u) | (TARG(w) << 2);
                   is_jump = 1; taken = 1; ctx->r[31] = pc + 8; break;

        case 0x04: branch_to = pc + 4 + (SIMM(w) << 2);
                   taken = ctx->r[RS(w)] == ctx->r[RT(w)]; break;
        case 0x05: branch_to = pc + 4 + (SIMM(w) << 2);
                   taken = ctx->r[RS(w)] != ctx->r[RT(w)]; break;
        case 0x06: branch_to = pc + 4 + (SIMM(w) << 2);
                   taken = (s32)ctx->r[RS(w)] <= 0; break;
        case 0x07: branch_to = pc + 4 + (SIMM(w) << 2);
                   taken = (s32)ctx->r[RS(w)] > 0; break;

        case 0x08: case 0x09:
            SET(RT(w), ctx->r[RS(w)] + SIMM(w)); break;
        case 0x0A: SET(RT(w), SLT(ctx->r[RS(w)], SIMM(w))); break;
        case 0x0B: SET(RT(w), SLTU(ctx->r[RS(w)], SIMM(w))); break;
        case 0x0C: SET(RT(w), ctx->r[RS(w)] & IMM(w)); break;
        case 0x0D: SET(RT(w), ctx->r[RS(w)] | IMM(w)); break;
        case 0x0E: SET(RT(w), ctx->r[RS(w)] ^ IMM(w)); break;
        case 0x0F: SET(RT(w), IMM(w) << 16); break;

        case 0x20: COOP_READ(ctx->r[RS(w)] + SIMM(w), 1); SET(RT(w), MEM_R8S(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x21: COOP_READ(ctx->r[RS(w)] + SIMM(w), 2); SET(RT(w), MEM_R16S(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x22: COOP_READ(ctx->r[RS(w)] + SIMM(w), 4); SET(RT(w), MEM_LWL(ctx->r[RT(w)], ctx->r[RS(w)] + SIMM(w))); break;
        case 0x23: COOP_READ(ctx->r[RS(w)] + SIMM(w), 4); SET(RT(w), MEM_R32(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x24: COOP_READ(ctx->r[RS(w)] + SIMM(w), 1); SET(RT(w), MEM_R8U(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x25: COOP_READ(ctx->r[RS(w)] + SIMM(w), 2); SET(RT(w), MEM_R16U(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x26: COOP_READ(ctx->r[RS(w)] + SIMM(w), 4); SET(RT(w), MEM_LWR(ctx->r[RT(w)], ctx->r[RS(w)] + SIMM(w))); break;

        case 0x28: MEM_W8(ctx->r[RS(w)] + SIMM(w), ctx->r[RT(w)]); break;
        case 0x29: MEM_W16(ctx->r[RS(w)] + SIMM(w), ctx->r[RT(w)]); break;
        case 0x2A: MEM_SWL(ctx->r[RS(w)] + SIMM(w), ctx->r[RT(w)]); break;
        case 0x2B: MEM_W32(ctx->r[RS(w)] + SIMM(w), ctx->r[RT(w)]); break;
        case 0x2E: MEM_SWR(ctx->r[RS(w)] + SIMM(w), ctx->r[RT(w)]); break;

        case 0x10:
            if (RS(w) == 0x00)      SET(RT(w), ac3_mfc0(ctx, RD(w)));
            else if (RS(w) == 0x04) ac3_mtc0(ctx, RD(w), ctx->r[RT(w)]);
            else if (FUNCT(w) == 0x10) ac3_rfe(ctx);
            break;

        case 0x12:
            if (w & 0x02000000) {
                ac3_gte_op(ctx, w & 0x01FFFFFF);
            } else {
                switch (RS(w)) {
                case 0x00: SET(RT(w), ac3_gte_mfc2(ctx, RD(w))); break;
                case 0x02: SET(RT(w), ac3_gte_cfc2(ctx, RD(w))); break;
                case 0x04: ac3_gte_mtc2(ctx, RD(w), ctx->r[RT(w)]); break;
                case 0x06: ac3_gte_ctc2(ctx, RD(w), ctx->r[RT(w)]); break;
                default: break;
                }
            }
            break;

        case 0x32: COOP_READ(ctx->r[RS(w)] + SIMM(w), 4); ac3_gte_mtc2(ctx, RT(w),
                                MEM_R32(ctx->r[RS(w)] + SIMM(w))); break;
        case 0x3A: MEM_W32(ctx->r[RS(w)] + SIMM(w),
                           ac3_gte_mfc2(ctx, RT(w))); break;

        default:
            ac3_probe_unknown_insn(pc, w);
            break;
        }

        x->taken = taken;
        x->is_jump = is_jump;
        x->target = branch_to;
    }
}

u32 ac3_interp_pc;

u32 ac3_interp_step_budget;
int ac3_interp_budget_hit;

static void interp_run(ctx_t *ctx, u32 pc, u32 entry);

/* Address of the game's setjmp (the Psy-Q function replaced by psyq_setjmp),
 * found once in the dispatch table; 1 (never a valid target) if absent. */
extern const struct { u32 addr; ac3_func_t fn; } ac3_func_table[];
extern const unsigned ac3_func_table_len;
static u32 setjmp_addr(void) {
    static u32 a;
    if (!a) {
        a = 1;
        for (unsigned i = 0; i < ac3_func_table_len; i++)
            if (ac3_func_table[i].fn == psyq_setjmp) { a = ac3_func_table[i].addr; break; }
    }
    return a;
}

/* The game's setjmp. Kept out of interp_run on purpose: with Emscripten's
 * setjmp support, every call made from a function that contains setjmp() goes
 * through a JavaScript wrapper, which cost ~40% of the interpreter's time when
 * the setjmp sat in the main loop. Here, only this rare call pays for it.
 *
 * The C setjmp is taken in this frame, which stays alive while the rest of the
 * guest function runs (interp_run below, until that function returns) -- the
 * same thing tools/recompile.py emits at a recompiled call site.
 * psyq_longjmp restores ra/sp/fp/s0-s7/gp from the guest buffer, sets v0 and
 * longjmps back here; interpretation resumes at the restored return address. */
static void interp_setjmp_then_run(ctx_t *ctx, u32 resume_pc, u32 entry) {
    int slot = ac3_setjmp_save(ctx, ctx->r[4]);
    u32 pc;
    if (setjmp(*ac3_setjmp_env(slot)) == 0) {
        ctx->r[2] = 0;
        pc = resume_pc;
    } else {
        pc = ctx->r[31];
    }
    interp_run(ctx, pc, entry);
}

void ac3_interpret(ctx_t *ctx, u32 entry) {
    u32 saved_ra = ctx->r[31];
    u32 outer_pc = ac3_interp_pc;
    ctx->r[31] = INTERP_SENTINEL;
    ac3_interp_budget_hit = 0;
    interp_run(ctx, entry, entry);
    ctx->r[31] = saved_ra;
    ac3_interp_pc = outer_pc;
}

/* Runs guest code from pc until the function entered at `entry` returns
 * (jr to the sentinel), or it tail-jumps into the BIOS or a native function. */
static void interp_run(ctx_t *ctx, u32 pc, u32 entry) {
    for (u64 steps = 0; steps < MAX_STEPS; steps++) {
        if (ac3_interp_step_budget && steps >= ac3_interp_step_budget) {
            ac3_interp_budget_hit = 1;
            return;
        }
        xfer_t x;
        ac3_interp_pc = pc;
        if (((pc ^ 0x800F0000u) & 0xFFFFF000u) == 0)   /* the probe only watches this page */
            ac3_probe_interp_pc(ctx, pc);
        exec_one(ctx, MEM_R32(pc), pc, &x);
        if (!x.taken) {
            pc += 4;
            continue;
        }

        {
            xfer_t d;
            exec_one(ctx, MEM_R32(pc + 4), pc + 4, &d);
            if (d.taken)
                fprintf(stderr, "ac3: interp: branch in delay slot at 0x%08X\n",
                        pc + 4);
        }

        u32 op = OP(x.word), fn = FUNCT(x.word);
        int is_call = (op == 0x03) || (op == 0x00 && fn == 0x09);
        int is_ret  = (op == 0x00 && fn == 0x08);

        if (is_call) {
            if (x.target == setjmp_addr()) {
                interp_setjmp_then_run(ctx, pc + 8, entry);
                return;                      /* the guest function has returned */
            }
            ac3_guest_ra = pc + 8;           /* the return address the guest would see */
            call_at(ctx, x.target);
            pc += 8;
            continue;
        }
        if (is_ret) {
            if (x.target == INTERP_SENTINEL) return;
            if (x.target == 0xA0u || x.target == 0xB0u || x.target == 0xC0u) {
                ac3_bios_call(ctx, x.target, ctx->r[9]);
                return;
            }
            ac3_func_t f = ac3_lookup(x.target);
            if (f) {
                f(ctx);
                return;
            }
            if (x.target < 0x10000u) {
                fprintf(stderr, "ac3: interp: jr to implausible 0x%08X "
                        "from 0x%08X\n", x.target, pc);
                return;
            }
            pc = x.target;
            continue;
        }
        pc = x.target;
    }
    fprintf(stderr, "ac3: interp: step limit reached from 0x%08X\n", entry);
}
