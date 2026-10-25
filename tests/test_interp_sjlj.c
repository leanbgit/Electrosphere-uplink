/*
 * test_interp_sjlj.c -- unit test: the MIPS interpreter's handling of the
 * game's setjmp/longjmp (runtime/ac3_interp.c + runtime/hle/psyq_libc.c).
 *
 * Builds natively against the runtime with its own two-entry dispatch table
 * (setjmp/longjmp at made-up addresses). Run: tests/run_tests.sh
 *
 * Program (hand-assembled, run by ac3_interpret):
 *   main:  addiu sp,-16 ; sw ra,12(sp) ; sw s0,8(sp)
 *          ori s0,0x1111 ; lui a0,0x8010          # jmp_buf at 0x80100000
 *          jal SETJMP ; nop
 *          (increment a counter in RAM)          # counts arrivals here;
 *                                                # not a register: longjmp
 *                                                # restores s-registers
 *          bnez v0,done ; nop
 *          ori s0,0x2222                         # clobber before jumping
 *          jal inner ; nop
 *          b fail ; nop                          # inner never returns
 *   done:  or s2,v0,zero
 *   fail:  lw s0,8(sp) ; lw ra,12(sp) ; jr ra ; addiu sp,16
 *   inner: addiu sp,-32 ; lui a0,0x8010 ; ori a1,5 ; jal LONGJMP ; nop ; jr ra ; nop
 */
#include "ac3_runtime.h"
#include <stdio.h>

void psyq_setjmp(ctx_t *ctx);
void psyq_longjmp(ctx_t *ctx);
const struct { u32 addr; ac3_func_t fn; } ac3_func_table[] = {
    { 0x80060000u, psyq_setjmp }, { 0x80060100u, psyq_longjmp },
};
const unsigned ac3_func_table_len = 2;
unsigned long long ac3_prof[1];
const unsigned ac3_prof_len = 0;
extern void ac3_mem_init(void);

#define JAL(t)        (0x0C000000u | (((t) >> 2) & 0x03FFFFFFu))
#define ADDIU(t,s,i)  (0x24000000u | ((s) << 21) | ((t) << 16) | ((i) & 0xFFFFu))
#define LUI(t,i)      (0x3C000000u | ((t) << 16) | ((i) & 0xFFFFu))
#define ORI(t,s,i)    (0x34000000u | ((s) << 21) | ((t) << 16) | ((i) & 0xFFFFu))
#define SW(t,o,b)     (0xAC000000u | ((b) << 21) | ((t) << 16) | ((o) & 0xFFFFu))
#define LW(t,o,b)     (0x8C000000u | ((b) << 21) | ((t) << 16) | ((o) & 0xFFFFu))
#define BNE(s,t,off)  (0x14000000u | ((s) << 21) | ((t) << 16) | ((off) & 0xFFFFu))
#define BEQ(s,t,off)  (0x10000000u | ((s) << 21) | ((t) << 16) | ((off) & 0xFFFFu))
#define OR(d,s,t)     (((s) << 21) | ((t) << 16) | ((d) << 11) | 0x25u)
#define JR(s)         (((s) << 21) | 0x08u)
#define NOP           0u
enum { ZERO=0, V0=2, A0=4, A1=5, T0=8, T1=9, S0=16, S1=17, S2=18, SP=29, RA=31 };

int main(void) {
    ac3_mem_init();
    const u32 base = 0x80050000u, inner = base + 23 * 4;
    const u32 code[] = {
        ADDIU(SP,SP,-16), SW(RA,12,SP), SW(S0,8,SP),          /*  0- 2 */
        ORI(S0,ZERO,0x1111), LUI(A0,0x8010),                   /*  3- 4 */
        JAL(0x80060000u), NOP,                                 /*  5- 6 */
        LUI(T0,0x8010), LW(T1,0x100,T0), ADDIU(T1,T1,1), SW(T1,0x100,T0), /* 7-10 count in RAM */
        BNE(V0,ZERO,18-12), NOP,                               /* 11-12  -> 18 */
        ORI(S0,ZERO,0x2222),                                   /* 13    */
        JAL(inner), NOP,                                       /* 14-15 */
        BEQ(ZERO,ZERO,19-17), NOP,                             /* 16-17  -> 19 */
        OR(S2,V0,ZERO),                                        /* 18 done */
        LW(S0,8,SP), LW(RA,12,SP), JR(RA), ADDIU(SP,SP,16),    /* 19-22 fail/epilogue */
        ADDIU(SP,SP,-32), LUI(A0,0x8010), ORI(A1,ZERO,5),      /* 23-25 inner */
        JAL(0x80060100u), NOP, JR(RA), NOP,                    /* 26-29 */
    };
    for (unsigned i = 0; i < sizeof code / 4; i++) MEM_W32(base + 4 * i, code[i]);

    ctx_t ctx = {0};
    ctx.r[SP] = 0x801FFF00u;
    ac3_interpret(&ctx, base);

    int fails = 0;
    #define CHECK(name, got, want) do { int ok = (u32)(got) == (u32)(want); fails += !ok; \
        printf("  %-40s 0x%08X  %s\n", name, (u32)(got), ok ? "ok" : "WRONG"); } while (0)
    printf("interpreter setjmp/longjmp\n");
    CHECK("code after setjmp ran (times, RAM)",   MEM_R32(0x80100100u), 2);
    CHECK("setjmp's second result (longjmp 5)",   ctx.r[S2], 5);
    CHECK("sp back at its starting value",        ctx.r[SP], 0x801FFF00u);
    CHECK("s0 restored by main's epilogue",       ctx.r[S0], 0);
    CHECK("jmp_buf holds s0 at setjmp (0x1111)",  MEM_R32(0x80100000u + 4u * 3u), 0x1111);
    printf(fails ? "SELFTEST FAILED (%d)\n" : "SELFTEST PASSED (0 failures)\n", fails);
    return fails != 0;
}
