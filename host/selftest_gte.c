#include "ac3_runtime.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void chk(const char *what, u32 got, u32 want) {
    int ok = (got == want);
    printf("  %-32s 0x%-10X %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected 0x%X (%d)\n", want, (s32)want); failures++; }
}

static void chks(const char *what, s32 got, s32 want) {
    int ok = (got == want);
    printf("  %-32s %-12d %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected %d\n", want); failures++; }
}

#define D(r)    ac3_gte_mfc2(&ctx, (r))
#define C(r)    ac3_gte_cfc2(&ctx, (r))
#define SETD(r, v) ac3_gte_mtc2(&ctx, (r), (u32)(v))
#define SETC(r, v) ac3_gte_ctc2(&ctx, (r), (u32)(v))
#define OP(x)   ac3_gte_op(&ctx, (x))

int ac3_selftest_gte(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    failures = 0;
    printf("GTE self-test\n");

    SETC(0, 0x00001000); SETC(1, 0); SETC(2, 0x00001000);
    SETC(3, 0); SETC(4, 0x1000);
    SETC(5, 0); SETC(6, 0); SETC(7, 0);
    SETC(24, 0); SETC(25, 0);
    SETC(26, 256);
    SETC(27, 256); SETC(28, 0);
    SETD(0, (200u << 16) | 100u);
    SETD(1, 512);

    OP(0x00080001);

    printf("  --- RTPS (identity, H=256, Z=512) ---\n");
    chks("MAC1", (s32)D(25), 100);
    chks("MAC2", (s32)D(26), 200);
    chks("MAC3", (s32)D(27), 512);
    chks("IR1",  (s32)D(9), 100);
    chks("IR3",  (s32)D(11), 512);
    chks("SZ3",  (s32)D(19), 512);
    chk ("SXY2 (SX=50, SY=100)", D(14), (100u << 16) | 50u);
    chks("IR0 (depth cue)", (s32)D(8), 2048);
    chk ("FLAG clean", C(31), 0);

    printf("  --- divide overflow (H=1000, Z=100) ---\n");
    SETC(26, 1000);
    SETD(1, 100);
    OP(0x00080001);
    chk("FLAG divide-overflow bit", C(31) & (1u << 17), 1u << 17);
    chk("FLAG error bit 31 set", C(31) & 0x80000000u, 0x80000000u);

    printf("  --- NCLIP ---\n");
    SETD(12, 0);
    SETD(13, (0u << 16) | 10u);
    SETD(14, (10u << 16) | 0u);
    OP(0x06);
    chks("MAC0 (counter-clockwise)", (s32)D(24), 100);

    SETD(13, (10u << 16) | 0u);
    SETD(14, (0u << 16) | 10u);
    OP(0x06);
    chks("MAC0 (clockwise, negated)", (s32)D(24), -100);

    printf("  --- AVSZ3 ---\n");
    SETC(29, 4096);
    SETD(17, 100); SETD(18, 100); SETD(19, 100);
    OP(0x2D);
    chks("MAC0", (s32)D(24), 1228800);
    chks("OTZ",  (s32)D(7), 300);

    printf("  --- SQR with IR saturation ---\n");
    SETD(9, 100); SETD(10, 200); SETD(11, 300);
    OP(0x28);
    chks("MAC1 = 100*100", (s32)D(25), 10000);
    chks("MAC2 = 200*200", (s32)D(26), 40000);
    chks("IR1 (in range)", (s32)D(9), 10000);
    chks("IR2 (clamped)",  (s32)D(10), 32767);
    chks("IR3 (clamped)",  (s32)D(11), 32767);
    chk ("FLAG IR2 saturated", C(31) & (1u << 23), 1u << 23);
    chk ("FLAG IR3 saturated", C(31) & (1u << 22), 1u << 22);

    printf("  --- lm bit clamps IR at zero ---\n");
    SETD(9, -100); SETD(10, 0); SETD(11, 0);
    SETD(8, -1);
    OP(0x0000003D);
    chks("GPF lm=0 keeps negative", (s32)D(9), 100);
    SETD(9, 100); SETD(8, -1);
    OP(0x00000400 | 0x3D);
    chks("GPF lm=1 clamps to 0", (s32)D(9), 0);

    printf("  --- LZCS / LZCR ---\n");
    SETD(30, 0x00000F00); chks("lzcr(0x00000F00)", (s32)D(31), 20);
    SETD(30, 0xFFFFF0FF); chks("lzcr(0xFFFFF0FF)", (s32)D(31), 20);
    SETD(30, 0);          chks("lzcr(0)", (s32)D(31), 32);
    SETD(30, 0xFFFFFFFF); chks("lzcr(0xFFFFFFFF)", (s32)D(31), 32);
    SETD(30, 0x80000000); chks("lzcr(0x80000000)", (s32)D(31), 1);

    printf("  --- register file ---\n");
    SETC(26, 0x8000);
    chk("cfc2 H sign-extends", C(26), 0xFFFF8000u);

    SETD(12, 0x11111111); SETD(13, 0x22222222); SETD(14, 0x33333333);
    chk("mfc2 SXYP mirrors SXY2", D(15), 0x33333333u);
    SETD(15, 0x44444444);
    chk("SXYP push -> SXY0", D(12), 0x22222222u);
    chk("SXYP push -> SXY1", D(13), 0x33333333u);
    chk("SXYP push -> SXY2", D(14), 0x44444444u);

    SETD(28, (3u << 10) | (2u << 5) | 1u);
    chks("IRGB write -> IR1", (s32)D(9), 1 * 0x80);
    chks("IRGB write -> IR2", (s32)D(10), 2 * 0x80);
    chks("IRGB write -> IR3", (s32)D(11), 3 * 0x80);
    chk ("ORGB read back", D(29), (3u << 10) | (2u << 5) | 1u);

    printf("%s (%d failures)\n",
           failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    printf("note: covers the paths AC3 relies on most, not all 22 opcodes.\n"
           "      Full bit-exactness needs a differential run against a\n"
           "      validated GTE core -- tracked as task #7.\n");
    return failures ? 1 : 0;
}
