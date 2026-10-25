#include "hle.h"
#include "ac3_capture.h"
#include <stdio.h>
#include <string.h>

extern int  ac3_disc_open(const char *path, int number);
extern int  ac3_disc_attach(const char *path);
extern int  ac3_disc_other_path(const char *path, char *out, size_t n);
extern int  ac3_disc_number(void);
extern void psyq_CdIntToPos(ctx_t *ctx);
extern void psyq_CdGetSector(ctx_t *ctx);
extern void psyq_CdInit(ctx_t *ctx);
extern void psyq_CdSearchFile(ctx_t *ctx);
extern void psyq_CdControl(ctx_t *ctx);
extern void psyq_CdRead(ctx_t *ctx);
extern void psyq_CdPosToInt(ctx_t *ctx);

#define NAME_BUF 0x80130000u
#define FILE_BUF 0x80130100u
#define LOC_BUF  0x80130200u
#define MODE_BUF 0x80130210u
#define HDR_BUF  0x80130220u
#define DATA_BUF 0x80140000u

static int failures;

static int check_u32(const char *what, u32 got, u32 want) {
    int ok = (got == want);
    printf("  %-30s %-12u %s\n", what, got, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected %u\n", want); failures++; }
    return ok;
}

static void put_name(u32 addr, const char *s) {
    u32 i = 0;
    for (; s[i]; i++) MEM_W8(addr + i, (u8)s[i]);
    MEM_W8(addr + i, 0);
}

static int lookup(ctx_t *ctx, const char *name, u32 *lba, u32 *size) {
    put_name(NAME_BUF, name);
    ctx->r[4] = FILE_BUF;
    ctx->r[5] = NAME_BUF;
    psyq_CdSearchFile(ctx);
    if (ctx->r[2] == 0) return 0;
    ctx->r[4] = FILE_BUF;
    psyq_CdPosToInt(ctx);
    *lba = ctx->r[2];
    *size = MEM_R32(FILE_BUF + 4);
    return 1;
}

#define SPH_LBA    609u
#define SPB_LBA    46986u
#define SPH_COUNT  487u
#define ON_DISC    0x80000000u

#define KCD_READ_INDEX  0x8002AE0Cu
#define KCD_DISC_STATUS 0x8002B504u
#define STREAM_ON_DISC  0x8002CB58u
#define GAME_GP         0x800BCB74u
#define KCD_STATE       0x80180000u
#define KCD_BUFFER      0x80190000u
#define GAME_STACK      0x801FF000u

static int raw_user(FILE *f, u32 lba, u8 *out) {
    if (fseek(f, (long)lba * 2352 + 24, SEEK_SET) != 0) return 0;
    return fread(out, 1, 2048, f) == 2048;
}

static u32 rd32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void seek_to(ctx_t *ctx, u32 lba) {
    ctx->r[4] = lba; ctx->r[5] = LOC_BUF;
    psyq_CdIntToPos(ctx);
    ctx->r[4] = 0x02; ctx->r[5] = LOC_BUF; ctx->r[6] = 0;
    psyq_CdControl(ctx);
}

static int read_one(ctx_t *ctx, u32 lba, u32 dst) {
    seek_to(ctx, lba);
    ctx->r[4] = 1; ctx->r[5] = dst; ctx->r[6] = 0;
    psyq_CdRead(ctx);
    return (int)ctx->r[2];
}

static void set_mode(ctx_t *ctx, u8 mode) {
    MEM_W8(MODE_BUF, mode);
    ctx->r[4] = 0x0E; ctx->r[5] = MODE_BUF; ctx->r[6] = 0;
    psyq_CdControl(ctx);
}

static void call_game(ctx_t *ctx, u32 addr, u32 a0) {
    ac3_func_t fn = ac3_lookup(addr);
    ctx->r[4] = a0;
    ctx->r[28] = GAME_GP;
    ctx->r[29] = GAME_STACK;
    ctx->r[31] = 0;
    if (fn) fn(ctx);
    else { printf("  0x%08X is not a recompiled function\n", addr); failures++; }
}

static int load_game(ctx_t *ctx) {
    u32 lba = 0, size = 0;
    if (!lookup(ctx, "ACE3.BIN", &lba, &size) || !read_one(ctx, lba, DATA_BUF))
        return 0;
    u32 dest = MEM_R32(DATA_BUF + 0x18), text = MEM_R32(DATA_BUF + 0x1C);
    seek_to(ctx, lba + 1);
    ctx->r[4] = (text + 2047) / 2048; ctx->r[5] = dest; ctx->r[6] = 0;
    psyq_CdRead(ctx);
    return ctx->r[2] == 1;
}

static void both_discs(ctx_t *ctx, const char *image, const char *image2) {
    char other[1024];
    if (!image2 || !*image2)
        image2 = ac3_disc_other_path(image, other, sizeof other) ? other : NULL;
    if (!image2) {
        printf("  --- both discs: no second image beside this one, skipped ---\n");
        return;
    }
    printf("  --- both discs: %s ---\n", image2);

    FILE *f1 = fopen(image, "rb"), *f2 = fopen(image2, "rb");
    if (!f1 || !f2) { printf("  cannot open the images\n"); failures++; goto out; }
    fseek(f1, 0, SEEK_END);
    u32 base = (u32)(ftell(f1) / 2352);

    static u8 idx1[4 * 2048], idx2[4 * 2048];
    for (u32 i = 0; i < 4; i++)
        if (!raw_user(f1, SPH_LBA + i, idx1 + i * 2048) ||
            !raw_user(f2, SPH_LBA + i, idx2 + i * 2048)) {
            printf("  cannot read the raw indices\n"); failures++; goto out;
        }
    int n1 = (int)rd32(idx1 + 4), n2 = (int)rd32(idx2 + 4);
    check_u32("raw index entries", rd32(idx1 + 16), SPH_COUNT);

    ac3_disc_open(image, n1);
    check_u32("attach same disc refused", (u32)ac3_disc_attach(image), 0);
    check_u32("  disc number unchanged", (u32)ac3_disc_number(), (u32)n1);
    read_one(ctx, SPH_LBA, DATA_BUF);
    check_u32("  index disc field", MEM_R32(DATA_BUF + 4), (u32)n1);

    ac3_disc_open(image, n1);
    if (!check_u32("attach other disc", (u32)ac3_disc_attach(image2), 1)) goto out;
    check_u32("disc number seen", (u32)ac3_disc_number(), 3);

    if (!check_u32("ACE3.BIN loaded", (u32)load_game(ctx), 1)) goto out;
    memset(ac3_ram + (KCD_STATE & 0x1FFFFFu), 0, 0x100);
    MEM_W32(GAME_GP + 52, KCD_STATE);
    MEM_W32(GAME_GP + 68, KCD_BUFFER);
    call_game(ctx, KCD_READ_INDEX, 0);
    check_u32("KCd index read", ctx->r[2], 1);
    check_u32("KCd disc status", MEM_R32(KCD_STATE + 12), 3);
    call_game(ctx, KCD_DISC_STATUS, 0);
    check_u32("kcd_disc_status()", ctx->r[2], 3);

    u32 table = MEM_R32(KCD_STATE + 28);
    u32 not_here = 0, bad_entry = 0, bad_data = 0, bad_header = 0;
    u32 from1 = 0, from2 = 0;
    static u8 want[2048];
    ac3_cd_snap_t snap;
    for (u32 k = 0; k < SPH_COUNT; k++) {
        call_game(ctx, STREAM_ON_DISC, 0x800u | k);
        if (ctx->r[2] != 1) not_here++;

        u32 packed = MEM_R32(table + 8 * k), lba = MEM_R32(table + 8 * k + 4);
        const u8 *e1 = idx1 + 20 + 8 * k, *e2 = idx2 + 20 + 8 * k;
        FILE *src;
        u32 src_lba, want_packed, want_lba;
        if (rd32(e1) & ON_DISC) {
            src = f1; src_lba = SPB_LBA + rd32(e1 + 4);
            want_packed = rd32(e1); want_lba = src_lba;
            from1++;
        } else {
            src = f2; src_lba = SPB_LBA + rd32(e2 + 4);
            want_packed = rd32(e2); want_lba = base + src_lba;
            from2++;
        }
        if (packed != want_packed || lba != want_lba) {
            if (!bad_entry)
                printf("      entry %u: 0x%08X @%u, expected 0x%08X @%u\n",
                       k, packed, lba, want_packed, want_lba);
            bad_entry++;
            continue;
        }
        if (!read_one(ctx, lba, DATA_BUF) || !raw_user(src, src_lba, want) ||
            memcmp(ac3_ram + (DATA_BUF & 0x1FFFFFu), want, 2048) != 0) {
            bad_data++;
            continue;
        }
        if (src == f2) {
            set_mode(ctx, 0x20);
            seek_to(ctx, lba);
            ctx->r[4] = 0x06; ctx->r[5] = 0; ctx->r[6] = 0;
            psyq_CdControl(ctx);
            ctx->r[4] = HDR_BUF; ctx->r[5] = 3;
            psyq_CdGetSector(ctx);
            ac3_cd_snapshot(&snap);
            ctx->r[4] = HDR_BUF;
            psyq_CdPosToInt(ctx);
            if (ctx->r[2] != snap.fifo_lba || snap.fifo_lba < lba) {
                if (!bad_header)
                    printf("      entry %u: header says %d, drive at %u\n",
                           k, (int)ctx->r[2], snap.fifo_lba);
                bad_header++;
            }
            ctx->r[4] = 0x09; ctx->r[5] = 0; ctx->r[6] = 0;
            psyq_CdControl(ctx);
            set_mode(ctx, 0);
        }
    }
    printf("  streams: %u from disc %d, %u from disc %d\n", from1, n1, from2, n2);
    check_u32("streams not on this disc", not_here, 0);
    check_u32("index entries wrong", bad_entry, 0);
    check_u32("stream data wrong", bad_data, 0);
    check_u32("sector headers wrong", bad_header, 0);

out:
    if (f1) fclose(f1);
    if (f2) fclose(f2);
}

extern int  ac3_disc_english_active(void);
extern int  ac3_disc_pack_split(void);
extern int  ac3_disc_pack_number(void);
extern int  ac3_disc_pack_follow(int number);

#define PACK_MAX 1021u

static struct {
    u32 pack, count;
    u8  index[4 * 2048];
    u8  first[PACK_MAX][2048];
} alone[2];

static int read_alone(ctx_t *ctx, const char *image, int i) {
    u32 lba = 0, size = 0, pack_size = 0;
    if (!ac3_disc_open(image, i + 1) || !ac3_disc_english_active()) return 0;
    if (!lookup(ctx, "ACE.BPH", &lba, &size) || size > sizeof alone[i].index ||
        !lookup(ctx, "ACE.BPB", &alone[i].pack, &pack_size))
        return 0;
    for (u32 s = 0; s * 2048 < size; s++) {
        if (!read_one(ctx, lba + s, DATA_BUF)) return 0;
        memcpy(alone[i].index + s * 2048, ac3_ram + (DATA_BUF & 0x1FFFFFu), 2048);
    }
    alone[i].count = rd32(alone[i].index + 16);
    if (alone[i].count > PACK_MAX) return 0;
    for (u32 k = 0; k < alone[i].count; k++) {
        if (!read_one(ctx, alone[i].pack + rd32(alone[i].index + 24 + 8 * k), DATA_BUF)) return 0;
        memcpy(alone[i].first[k], ac3_ram + (DATA_BUF & 0x1FFFFFu), 2048);
    }
    return 1;
}

static int pack_entry(u32 table, u32 k, u32 *base) {
    u32 packed = MEM_R32(table + 8 * k), lba = MEM_R32(table + 8 * k + 4);
    const u8 *e0 = alone[0].index + 20 + 8 * k, *e1 = alone[1].index + 20 + 8 * k;
    if (packed == rd32(e0) && lba == alone[0].pack + rd32(e0 + 4)) return 0;
    u32 b = lba - alone[1].pack - rd32(e1 + 4);
    if (!*base) *base = b;
    return packed == rd32(e1) && b == *base && b >= alone[0].pack ? 1 : -1;
}

static u32 pack_table(u32 table, u8 *from, u32 *wrong) {
    u32 other = 0, base = 0;
    *wrong = 0;
    for (u32 k = 0; k < alone[0].count; k++) {
        int d = pack_entry(table, k, &base);
        if (d < 0) { (*wrong)++; continue; }
        from[k] = (u8)d;
        other += (u32)d;
    }
    return other;
}

static u32 pack_data(ctx_t *ctx, u32 table) {
    u32 wrong = 0, base = 0;
    for (u32 k = 0; k < alone[0].count; k++) {
        if (!read_one(ctx, MEM_R32(table + 8 * k + 4), DATA_BUF)) { wrong++; continue; }
        int d = pack_entry(table, k, &base);
        if (d < 0 || memcmp(ac3_ram + (DATA_BUF & 0x1FFFFFu), alone[d].first[k], 2048) != 0)
            wrong++;
    }
    return wrong;
}

int ac3_selftest_english(const char *image, const char *image2) {
    static u8 from[2][PACK_MAX];
    char other[1024];
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    failures = 0;

    printf("English discs self-test: %s\n", image);
    if (!image2 || !*image2)
        image2 = ac3_disc_other_path(image, other, sizeof other) ? other : NULL;
    if (!image2) { printf("  no second image beside this one\n"); return 1; }
    if (!read_alone(&ctx, image, 0) || !read_alone(&ctx, image2, 1)) {
        printf("  the English patch did not apply to both discs\n");
        return 1;
    }
    check_u32("pack records", alone[0].count, alone[1].count);
    u32 differ = 0;
    for (u32 k = 0; k < alone[0].count; k++)
        if (rd32(alone[0].index + 20 + 8 * k) != rd32(alone[1].index + 20 + 8 * k) ||
            memcmp(alone[0].first[k], alone[1].first[k], 2048) != 0)
            differ++;
    printf("  records that differ in size or first sector: %u\n", differ);

    int n1 = (int)rd32(alone[0].index + 4), n2;
    if (!ac3_disc_open(image, 1) ||
        !check_u32("attach other disc", (u32)ac3_disc_attach(image2), 1))
        goto out;
    check_u32("pack differs by disc", (u32)ac3_disc_pack_split(), 1);
    n1 = ac3_disc_pack_number();
    n2 = 3 - n1;
    if (!check_u32("ACE3.BIN loaded", (u32)load_game(&ctx), 1)) goto out;
    memset(ac3_ram + (KCD_STATE & 0x1FFFFFu), 0, 0x100);
    MEM_W32(GAME_GP + 52, KCD_STATE);
    MEM_W32(GAME_GP + 68, KCD_BUFFER);
    call_game(&ctx, KCD_READ_INDEX, 0);
    check_u32("KCd index read", ctx.r[2], 1);

    u32 table = MEM_R32(KCD_STATE + 24), wrong;
    u32 before = pack_table(table, from[0], &wrong);
    printf("  records read from the other disc: %u\n", before);
    check_u32("  entries of neither disc", wrong, 0);
    check_u32("  record data wrong", pack_data(&ctx, table), 0);
    if (before == 0) { printf("  none: MISMATCH\n"); failures++; }

    ac3_disc_pack_follow(n1);
    check_u32("follow the other disc", (u32)ac3_disc_pack_follow(n2), 1);
    u32 after = pack_table(table, from[1], &wrong);
    printf("  records read from the other disc: %u\n", after);
    check_u32("  entries of neither disc", wrong, 0);
    check_u32("  record data wrong", pack_data(&ctx, table), 0);
    u32 moved = 0, back = 0, only_other = PACK_MAX, only_booted = PACK_MAX;
    for (u32 k = 0; k < alone[0].count; k++) {
        if (from[0][k] != from[1][k]) moved++;
        if (from[0][k] && only_other == PACK_MAX) only_other = k;
        if (!from[0][k] && !from[1][k] && only_booted == PACK_MAX &&
            rd32(alone[0].index + 20 + 8 * k) != rd32(alone[1].index + 20 + 8 * k))
            only_booted = k;
    }
    check_u32("  records that moved", moved, after - before);
    if (moved == 0) { printf("  none moved: MISMATCH\n"); failures++; }

    ac3_disc_pack_follow(n2);
    check_u32("follow the booted disc", (u32)ac3_disc_pack_follow(n1), 1);
    pack_table(table, from[1], &wrong);
    for (u32 k = 0; k < alone[0].count; k++)
        if (from[0][k] != from[1][k]) back++;
    check_u32("  records not back", back, 0);

    if (only_other < PACK_MAX && only_booted < PACK_MAX) {
        read_one(&ctx, MEM_R32(table + 8 * only_other + 4), DATA_BUF);
        check_u32("disc after the other's record", (u32)ac3_disc_pack_number(), (u32)n2);
        read_one(&ctx, MEM_R32(table + 8 * only_booted + 4), DATA_BUF);
        check_u32("disc after the booted's record", (u32)ac3_disc_pack_number(), (u32)n1);
    } else {
        printf("  no record of one disc only: MISMATCH\n");
        failures++;
    }

out:
    printf("%s (%d failures)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED",
           failures);
    return failures ? 1 : 0;
}

int ac3_selftest_cd(const char *image, const char *image2) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    failures = 0;

    printf("virtual disc self-test: %s\n", image);
    if (!ac3_disc_open(image, 1)) return 1;
    psyq_CdInit(&ctx);
    if (ctx.r[2] == 0) { printf("  CdInit failed\n"); return 1; }

    u32 boot_lba = 0, boot_size = 0;
    const char *boot = lookup(&ctx, "SLPS_020.21", &boot_lba, &boot_size)
                     ? "SLPS_020.21" : "SLPS_020.20";

    struct { const char *name; u32 lba, size; } expect[] = {
        { "SYSTEM.CNF",  23,       67 },
        { boot,          24,   481280 },
        { "ACE3.BIN",    259,  710656 },
        { "ACE.BPH",     606,    4612 },
        { "ACE.SPH",     609,    3916 },
        { "ACE.BPB",     611, 94976000 },
    };

    for (unsigned i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        u32 lba = 0, size = 0;
        printf("  --- %s ---\n", expect[i].name);
        if (!lookup(&ctx, expect[i].name, &lba, &size)) {
            printf("  CdSearchFile FAILED for %s\n", expect[i].name);
            failures++;
            continue;
        }
        check_u32("lba", lba, expect[i].lba);
        check_u32("size", size, expect[i].size);
    }

    printf("  --- path and version-suffix handling ---\n");
    u32 lba = 0, size = 0;
    if (lookup(&ctx, "\\ACE3.BIN;1", &lba, &size))
        check_u32("\\ACE3.BIN;1 lba", lba, 259);
    else { printf("  FAILED to resolve \\ACE3.BIN;1\n"); failures++; }

    printf("  --- CdControl(Setloc) + CdRead ---\n");
    put_name(NAME_BUF, "ACE3.BIN");
    ctx.r[4] = FILE_BUF; ctx.r[5] = NAME_BUF;
    psyq_CdSearchFile(&ctx);
    ctx.r[4] = 0x02;
    ctx.r[5] = FILE_BUF;
    ctx.r[6] = 0;
    psyq_CdControl(&ctx);
    ctx.r[4] = 1;
    ctx.r[5] = DATA_BUF;
    ctx.r[6] = 0;
    psyq_CdRead(&ctx);
    check_u32("CdRead returned", ctx.r[2], 1);

    char magic[9];
    for (int i = 0; i < 8; i++) magic[i] = (char)MEM_R8U(DATA_BUF + (u32)i);
    magic[8] = 0;
    int ok = strcmp(magic, "PS-X EXE") == 0;
    printf("  %-30s %-12s %s\n", "first sector magic", magic, ok ? "ok" : "MISMATCH");
    if (!ok) failures++;

    ctx.r[4] = 1; ctx.r[5] = DATA_BUF + 2048u; ctx.r[6] = 0;
    psyq_CdRead(&ctx);
    check_u32("sequential read ok", ctx.r[2], 1);

    both_discs(&ctx, image, image2);

    printf("%s (%d failures)\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED",
           failures);
    return failures ? 1 : 0;
}
