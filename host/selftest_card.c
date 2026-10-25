#include "hle.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define RMDIR(p) rmdir(p)
#endif

extern void psyq_open(ctx_t *ctx);
extern void psyq_close(ctx_t *ctx);
extern void psyq_read(ctx_t *ctx);
extern void psyq_write(ctx_t *ctx);
extern void psyq_lseek(ctx_t *ctx);
extern void psyq_firstfile(ctx_t *ctx);
extern void psyq_nextfile(ctx_t *ctx);
extern void psyq_OpenEvent(ctx_t *ctx);
extern void psyq_CloseEvent(ctx_t *ctx);
extern void psyq_EnableEvent(ctx_t *ctx);
extern void psyq_TestEvent(ctx_t *ctx);
extern void ac3_card_set_path(int slot, const char *path);

#define PATH   0x80150000u
#define BUF    0x80150100u
#define DIR    0x80158000u
#define STACK  0x80160000u

#define O_RDONLY 0x0001u
#define O_CREATE 0x0200u
#define O_NOWAIT 0x8000u
#define BLOCK    8192u

#define SwCARD      0xF4000001u
#define EvSpIOE     0x0004u
#define EvMdNOINTR  0x2000u

#define SCRATCH_DIR  "selftest_saves"
#define SCRATCH      SCRATCH_DIR "/card1.mcd"

static void scratch_clean(void) {
    remove(SCRATCH);
    RMDIR(SCRATCH_DIR);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

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

static void check_s32(const char *what, s32 got, s32 want) {
    int ok = got == want;
    printf("  %-34s %-12d %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected %d\n", want); failures++; }
}

static void check_str(const char *what, const char *got, const char *want) {
    int ok = strcmp(got, want) == 0;
    printf("  %-34s %-12s %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected \"%s\"\n", want); failures++; }
}

static void check_true(const char *what, int ok) {
    printf("  %-34s %-12s %s\n", what, ok ? "yes" : "no", ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static s32 call3(void (*fn)(ctx_t *), ctx_t *c, u32 a0, u32 a1, u32 a2) {
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
    fn(c);
    return (s32)c->r[2];
}

int ac3_selftest_card(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[29] = STACK;
    failures = 0;
    char buf[64];

    printf("memory card filesystem self-test\n");

    scratch_clean();
    ac3_card_set_path(0, SCRATCH);

    put_str(PATH, "bu00:*");
    check_s32("firstfile on empty card", call3(psyq_firstfile, &ctx, PATH, DIR, 0), 0);

    put_str(PATH, "bu00:ACE3-SAVE-01");
    s32 fd = call3(psyq_open, &ctx, PATH, O_CREATE | (2u << 16), 0);
    check_true("open with create returns an fd", fd >= 0);

    for (u32 i = 0; i < 64; i++) MEM_W8(BUF + i, (u8)(0xA0 + i));
    check_s32("lseek to just before the boundary",
              call3(psyq_lseek, &ctx, (u32)fd, BLOCK - 32, 0), (s32)(BLOCK - 32));
    check_s32("write across the block boundary",
              call3(psyq_write, &ctx, (u32)fd, BUF, 64), 64);

    check_s32("lseek back to the start",
              call3(psyq_lseek, &ctx, (u32)fd, 0, 0), 0);
    put_str(BUF + 256, "ELECTROSPHERE");
    check_s32("write at the start",
              call3(psyq_write, &ctx, (u32)fd, BUF + 256, 16), 16);
    check_s32("close", call3(psyq_close, &ctx, (u32)fd, 0, 0), fd);

    fd = call3(psyq_open, &ctx, PATH, 1, 0);
    check_true("reopen without create", fd >= 0);
    check_s32("size is the whole chain",
              call3(psyq_lseek, &ctx, (u32)fd, 0, 2), (s32)(2 * BLOCK));

    call3(psyq_lseek, &ctx, (u32)fd, 0, 0);
    memset((void *)guest_ptr(BUF + 512, 64), 0, 64);
    check_s32("read at the start",
              call3(psyq_read, &ctx, (u32)fd, BUF + 512, 16), 16);
    get_str(BUF + 512, buf, sizeof buf);
    check_str("bytes at the start survived", buf, "ELECTROSPHERE");

    call3(psyq_lseek, &ctx, (u32)fd, BLOCK - 32, 0);
    check_s32("read across the block boundary",
              call3(psyq_read, &ctx, (u32)fd, BUF + 512, 64), 64);
    int spanned = 1;
    for (u32 i = 0; i < 64; i++)
        if (MEM_R8U(BUF + 512 + i) != (u8)(0xA0 + i)) spanned = 0;
    check_true("bytes across the boundary survived", spanned);

    call3(psyq_lseek, &ctx, (u32)fd, (u32)(2 * BLOCK - 8), 0);
    check_s32("read clamped at end of file",
              call3(psyq_read, &ctx, (u32)fd, BUF + 512, 64), 8);
    call3(psyq_close, &ctx, (u32)fd, 0, 0);

    ctx.r[7] = 0;
    u32 ev = (u32)call3(psyq_OpenEvent, &ctx, SwCARD, EvSpIOE, EvMdNOINTR);
    call3(psyq_EnableEvent, &ctx, ev, 0, 0);
    check_s32("no event before any transfer",
              call3(psyq_TestEvent, &ctx, ev, 0, 0), 0);

    put_str(PATH, "bu00:ACE3-SAVE-01");
    fd = call3(psyq_open, &ctx, PATH, O_RDONLY | O_NOWAIT, 0);
    check_true("open O_NOWAIT returns an fd", fd >= 0);
    call3(psyq_read, &ctx, (u32)fd, BUF + 512, 128);
    check_s32("async read posts SwCARD/EvSpIOE",
              call3(psyq_TestEvent, &ctx, ev, 0, 0), 1);

    call3(psyq_lseek, &ctx, (u32)fd, 0, 0);
    call3(psyq_write, &ctx, (u32)fd, BUF + 512, 128);
    check_s32("async write posts SwCARD/EvSpIOE",
              call3(psyq_TestEvent, &ctx, ev, 0, 0), 1);
    call3(psyq_close, &ctx, (u32)fd, 0, 0);

    fd = call3(psyq_open, &ctx, PATH, O_RDONLY, 0);
    call3(psyq_read, &ctx, (u32)fd, BUF + 512, 128);
    check_s32("blocking read posts nothing",
              call3(psyq_TestEvent, &ctx, ev, 0, 0), 0);
    call3(psyq_close, &ctx, (u32)fd, 0, 0);
    call3(psyq_CloseEvent, &ctx, ev, 0, 0);

    put_str(PATH, "bu00:ACE3-SAVE-02");
    fd = call3(psyq_open, &ctx, PATH, O_CREATE | (1u << 16), 0);
    call3(psyq_close, &ctx, (u32)fd, 0, 0);

    put_str(PATH, "bu00:*");
    check_true("firstfile finds a save",
               call3(psyq_firstfile, &ctx, PATH, DIR, 0) == (s32)DIR);
    get_str(DIR, buf, sizeof buf);
    check_str("first entry name", buf, "ACE3-SAVE-01");
    check_s32("first entry size", (s32)MEM_R32(DIR + 0x18), (s32)(2 * BLOCK));
    check_true("nextfile finds the second",
               call3(psyq_nextfile, &ctx, DIR, 0, 0) == (s32)DIR);
    get_str(DIR, buf, sizeof buf);
    check_str("second entry name", buf, "ACE3-SAVE-02");
    check_s32("nextfile ends the walk", call3(psyq_nextfile, &ctx, DIR, 0, 0), 0);

    put_str(PATH, "bu00:NOSUCH*");
    check_s32("firstfile with no match",
              call3(psyq_firstfile, &ctx, PATH, DIR, 0), 0);

    put_str(PATH, "bu00:MISSING");
    check_s32("open without create on absent file",
              call3(psyq_open, &ctx, PATH, 1, 0), -1);

    printf("  --- persistence ---\n");
    check_true("card file created under a new directory", file_exists(SCRATCH));
    ac3_card_set_path(0, SCRATCH);

    put_str(PATH, "bu00:ACE3-SAVE-01");
    fd = call3(psyq_open, &ctx, PATH, 1, 0);
    check_true("save reopens after a reload", fd >= 0);
    memset((void *)guest_ptr(BUF + 512, 64), 0, 64);
    call3(psyq_read, &ctx, (u32)fd, BUF + 512, 16);
    get_str(BUF + 512, buf, sizeof buf);
    check_str("save contents survived the reload", buf, "ELECTROSPHERE");
    call3(psyq_close, &ctx, (u32)fd, 0, 0);

    printf("  --- on-card structure ---\n");
    FILE *f = fopen(SCRATCH, "rb");
    check_true("card image written", f != NULL);
    if (f) {
        static u8 img[128 * 1024];
        size_t got = fread(img, 1, sizeof img, f);
        fclose(f);
        check_s32("image size", (s32)got, 128 * 1024);
        check_true("signature", img[0] == 'M' && img[1] == 'C');

        int bad = 0;
        for (int fr = 0; fr < 64; fr++) {
            u8 x = 0;
            for (int i = 0; i < 127; i++) x ^= img[fr * 128 + i];
            if (x != img[fr * 128 + 127]) bad++;
        }
        check_s32("directory frames with a bad checksum", bad, 0);

        int used = 0, first = 0;
        for (int b = 1; b <= 15; b++)
            if ((img[b * 128] & 0xF0) == 0x50) used++;
        for (int b = 1; b <= 15; b++)
            if (img[b * 128] == 0x51) first++;
        check_s32("blocks in use", used, 3);
        check_s32("files on the card", first, 2);
    }

    scratch_clean();
    printf("%s (%d failures)\n",
           failures ? "SELFTEST FAILED" : "SELFTEST PASSED", failures);
    return failures ? 1 : 0;
}
