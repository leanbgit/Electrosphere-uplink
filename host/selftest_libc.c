#include "ac3_input.h"
#include <stdio.h>
#include <string.h>

extern void psyq_sprintf(ctx_t *ctx);
extern void psyq_memset(ctx_t *ctx);
extern void psyq_memcpy(ctx_t *ctx);
extern void psyq_strlen(ctx_t *ctx);
extern void psyq_strcmp(ctx_t *ctx);
extern void psyq_strcat(ctx_t *ctx);
extern void psyq_setjmp(ctx_t *ctx);
extern void psyq_longjmp(ctx_t *ctx);
extern void psyq_InitPAD2(ctx_t *ctx);
extern void psyq_StartPAD2(ctx_t *ctx);
extern void ac3_pad_update(ctx_t *ctx);

#define DST   0x80150000u
#define FMT   0x80150400u
#define STR   0x80150500u
#define JB    0x80150600u
#define PADB  0x80150700u
#define STACK 0x80160000u

static int failures;

static void put_str(u32 a, const char *s) {
    u32 i = 0;
    for (; s[i]; i++) MEM_W8(a + i, (u8)s[i]);
    MEM_W8(a + i, 0);
}

static void get_str(u32 a, char *out, int cap) {
    int i = 0;
    while (i < cap - 1) {
        u8 c = (u8)MEM_R8U(a + (u32)i);
        if (!c) break;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

static void check_str(const char *what, const char *got, const char *want) {
    int ok = strcmp(got, want) == 0;
    printf("  %-28s %-26s %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected \"%s\"\n", want); failures++; }
}

static void check_u32(const char *what, u32 got, u32 want) {
    int ok = got == want;
    printf("  %-28s 0x%-24X %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected 0x%X\n", want); failures++; }
}

int ac3_selftest_libc(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[29] = STACK;
    failures = 0;
    char buf[256];

    printf("libc / pad self-test\n");

    put_str(STR, "Electrosphere");
    ctx.r[4] = STR;
    psyq_strlen(&ctx);
    check_u32("strlen", ctx.r[2], 13);

    ctx.r[4] = DST; ctx.r[5] = 0x41; ctx.r[6] = 8;
    psyq_memset(&ctx);
    ctx.r[4] = DST; ctx.r[5] = STR; ctx.r[6] = 14;
    psyq_memcpy(&ctx);
    get_str(DST, buf, sizeof buf);
    check_str("memcpy result", buf, "Electrosphere");

    put_str(DST + 64, "Ace ");
    put_str(STR + 64, "Combat");
    ctx.r[4] = DST + 64; ctx.r[5] = STR + 64;
    psyq_strcat(&ctx);
    get_str(DST + 64, buf, sizeof buf);
    check_str("strcat result", buf, "Ace Combat");

    ctx.r[4] = STR; ctx.r[5] = STR;
    psyq_strcmp(&ctx);
    check_u32("strcmp equal", ctx.r[2], 0);

    put_str(FMT, "%d %s %04x %c %d");
    put_str(STR + 128, "hi");
    ctx.r[4] = DST;
    ctx.r[5] = FMT;
    ctx.r[6] = 42;
    ctx.r[7] = STR + 128;
    MEM_W32(STACK + 16, 255);
    MEM_W32(STACK + 20, 'A');
    MEM_W32(STACK + 24, 7);
    psyq_sprintf(&ctx);
    get_str(DST, buf, sizeof buf);
    check_str("sprintf mixed args", buf, "42 hi 00ff A 7");
    check_u32("sprintf return length", ctx.r[2], 14);

    put_str(FMT, "%d");
    ctx.r[4] = DST; ctx.r[5] = FMT; ctx.r[6] = (u32)-1234;
    psyq_sprintf(&ctx);
    get_str(DST, buf, sizeof buf);
    check_str("sprintf negative", buf, "-1234");

    printf("  --- non-local jump ---\n");
    ctx.r[16] = 0xC0DE0001u;
    {
        int slot = ac3_setjmp_save(&ctx, JB);
        ctx.r[2] = (u32)setjmp(*ac3_setjmp_env(slot));
    }
    if (ctx.r[2] == 0) {
        ctx.r[16] = 0xDEAD0000u;
        ctx.r[4] = JB;
        ctx.r[5] = 5;
        psyq_longjmp(&ctx);
        printf("  longjmp returned -- FAILED\n");
        failures++;
    }
    check_u32("setjmp result after longjmp", ctx.r[2], 5);
    check_u32("callee-saved $s0 restored", ctx.r[16], 0xC0DE0001u);

    printf("  --- pad buffer ---\n");
    ac3_pad[0].connected = 1;
    ac3_pad[0].type = AC3_PAD_DIGITAL;
    ac3_pad[0].buttons = AC3_BTN_CROSS | AC3_BTN_START;
    ctx.r[4] = PADB; ctx.r[5] = 34; ctx.r[6] = 0; ctx.r[7] = 0;
    psyq_InitPAD2(&ctx);
    psyq_StartPAD2(&ctx);

    check_u32("buf[0] connected", MEM_R8U(PADB + 0), 0x00);
    check_u32("buf[1] pad id", MEM_R8U(PADB + 1), AC3_PAD_DIGITAL);

    u32 composed = (MEM_R8U(PADB + 2) << 8) | MEM_R8U(PADB + 3);
    u32 pressed = (~composed) & 0xFFFFu;
    check_u32("PadRead composition", pressed, (1u << 11) | (1u << 6));
    printf("      (PADstart 1<<11 | PADRdown/cross 1<<6)\n");

    check_u32("port 2 absent", MEM_R8U(PADB + 0), 0x00);

    printf("%s (%d failures)\n",
           failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    return failures ? 1 : 0;
}
