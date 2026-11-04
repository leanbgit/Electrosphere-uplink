/*
 * cd_discs.c -- the discs on the drive.
 *
 * One disc, or both mounted as one drive: disc 2's sectors follow disc 1's.
 * The game finds its streams through index files (ACE.SPH for ACE.SPB, ACE.BPH
 * for ACE.BPB); with both discs mounted, the booted disc's index is merged
 * with the other's so that a stream on either disc is found, and the game
 * never asks for a disc change. With the English version, the records of
 * ACE.BPB that differ between the discs follow the disc being played.
 */
#include "cd_internal.h"
#include "ac3_oracle.h"
#include <string.h>
#include <stdlib.h>

static disc_image_t discs[2];

#define KCD_INDEX_ID     0x45334341u
#define KCD_SPH_CODE     0x19990427u
#define KCD_BPH_CODE     0x00000427u
#define KCD_DISC_EITHER  3u
#define KCD_ON_THIS_DISC 0x80000000u
#define KCD_SPH_HEADER   20u

#define KCD_SPH_MAX (4u * 2048u)
static u8  merged_index[KCD_SPH_MAX];      /* ACE.SPH with both discs' streams */
static u32 merged_index_lba, merged_index_size;

/* ACE.BPB records with the English version: the same on both discs, translated
 * on one only, or on both but differently (they follow the disc being played) */
enum { PACK_SAME, PACK_ONLY_BOOTED, PACK_ONLY_OTHER, PACK_BOTH };
static u8  pack_index[2][KCD_SPH_MAX];     /* each disc's ACE.BPH */
static u8  pack_merged[KCD_SPH_MAX];       /* what the game reads */
static u8  pack_kind[KCD_SPH_MAX / 8];
static u32 pack_index_lba, pack_index_size, pack_lba;
static int pack_disc;                      /* the disc the records follow */

static u32 rd32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void wr32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* ---- reading -------------------------------------------------------------- */

static const disc_image_t *image_for(u32 lba) {
    for (int i = 1; i >= 0; i--) {
        const disc_image_t *d = &discs[i];
        if (d->fp && lba >= d->base && (long)(lba - d->base) < d->sectors)
            return d;
    }
    return NULL;
}

/* the merged indexes replace the booted disc's own */
static void index_sector(u8 *out, u32 lba, const u8 *index, u32 index_lba, u32 index_size) {
    if (!index_size || lba < index_lba || lba - index_lba >= KCD_SPH_MAX / USER_SIZE) return;
    u32 off = (lba - index_lba) * USER_SIZE;
    if (off < index_size) {
        u32 n = index_size - off;
        memcpy(out + 12, index + off, n < USER_SIZE ? n : USER_SIZE);
    }
}

int read_sector_whole(u32 lba, u8 *out) {
    const disc_image_t *d = image_for(lba);
    if (!d || !image_read(d, lba - d->base, out)) return 0;

    if (d->base) lba_to_msf(lba, &out[0], &out[1], &out[2]);

    index_sector(out, lba, merged_index, merged_index_lba, merged_index_size);
    index_sector(out, lba, pack_merged, pack_index_lba, pack_index_size);
    return 1;
}

int read_sector(u32 lba, u8 *out) {
    u8 whole[WHOLE_SIZE];
    if (!read_sector_whole(lba, whole)) return 0;
    memcpy(out, whole + 12, USER_SIZE);
    return 1;
}

u32 drive_sectors(void) {
    if (discs[1].fp) return discs[1].base + (u32)discs[1].sectors;
    return (u32)discs[0].sectors;
}

int disc_mounted(void) {
    return discs[0].fp != NULL;
}

int find_file(const char *name, u32 *lba, u32 *size) {
    return iso_find(&discs[0], name, lba, size);
}

int ac3_disc_english_active(void) {
    return discs[0].en != NULL;
}

int ac3_disc_read_file(int which, const char *name, u8 *buf, u32 cap, u32 *size) {
    const disc_image_t *d = &discs[which & 1];
    u32 lba, sz;
    if (!d->fp || !iso_find(d, name, &lba, &sz) || sz > cap) return 0;
    u8 sec[USER_SIZE];
    for (u32 off = 0; off < sz; off += USER_SIZE) {
        if (!image_read_user(d, lba + off / USER_SIZE, sec)) return 0;
        u32 n = sz - off < USER_SIZE ? sz - off : USER_SIZE;
        memcpy(buf + off, sec, n);
    }
    *size = sz;
    return 1;
}

/* ---- both discs as one ---------------------------------------------------- */

/* an index file and the start of its pack; returns its number of records */
static int read_index(const disc_image_t *d, const char *index, const char *pack, u32 code,
                      u8 *buf, u32 *lba, u32 *size, u32 *pack_lba) {
    u32 pack_size;
    if (!iso_find(d, index, lba, size) ||
        !iso_find(d, pack, pack_lba, &pack_size))
        return -1;
    if (*size < KCD_SPH_HEADER || *size > KCD_SPH_MAX) return -1;
    memset(buf, 0, KCD_SPH_MAX);
    for (u32 s = 0; s * USER_SIZE < *size; s++)
        if (!image_read_user(d, *lba + s, buf + s * USER_SIZE)) return -1;
    if (rd32(buf) != KCD_INDEX_ID || rd32(buf + 8) != code) return -1;
    u32 count = rd32(buf + 16);
    if (KCD_SPH_HEADER + 8u * count > *size) return -1;
    return (int)count;
}

static u32 record_sectors(const u8 *entry) {
    return ((rd32(entry) & 0x00FFFFFFu) * 4u + USER_SIZE - 1) / USER_SIZE;
}

static int record_same(const disc_image_t *a, u32 ra, const disc_image_t *b, u32 rb,
                       u32 sectors) {
    u8 x[USER_SIZE], y[USER_SIZE];
    for (u32 s = 0; s < sectors; s++) {
        u32 from;
        if (a->en && !b->en && a->fp == b->fp &&
            !ac3_en_overlay_sector(a->en, ra + s, &from) && from == rb + s)
            continue;
        if (!image_read_user(a, ra + s, x) || !image_read_user(b, rb + s, y) ||
            memcmp(x, y, USER_SIZE) != 0)
            return 0;
    }
    return 1;
}

static void pack_serve(void) {
    u32 count = rd32(pack_index[0] + 16);
    memcpy(pack_merged, pack_index[0], KCD_SPH_MAX);
    for (u32 k = 0; k < count; k++) {
        int from = pack_kind[k] == PACK_ONLY_OTHER ? 1 : pack_kind[k] == PACK_BOTH ? pack_disc : 0;
        if (from)
            memcpy(pack_merged + KCD_SPH_HEADER + 8u * k,
                   pack_index[1] + KCD_SPH_HEADER + 8u * k, 8);
    }
}

/* Which ACE.BPB records differ between the discs' English versions takes
 * comparing their sectors: hundreds of MB read. The answer is kept beside the
 * booted disc ("<disc>.en.packs"), so this is done once; the website ships it
 * with the game data, so a player's first start does not download the whole
 * of ACE.BPB first. It is keyed by everything the answer depends on: both
 * discs' English and original indexes and the English versions' sizes. */
#define PACKS_MAGIC "AC3PACK1"
static u64 packs_key(const u8 *const idx[4], u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 4; i++)
        for (u32 k = 0; k < size; k++) { h ^= idx[i][k]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 2; i++) {
        u64 v[3] = { (u64)discs[i].sectors, (u64)discs[i].src_sectors,
                     discs[i].en ? (u64)ac3_en_overlay_literals(discs[i].en) : 0 };
        for (int j = 0; j < 3; j++) { h ^= v[j]; h *= 0x100000001B3ull; }
    }
    return h;
}
static int packs_load(u64 key, u32 count) {
    char path[1100];
    snprintf(path, sizeof path, "%s.en.packs", discs[0].path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8]; u64 k = 0; u32 n = 0;
    int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, PACKS_MAGIC, 8) &&
             fread(&k, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && k == key && n == count &&
             n <= sizeof pack_kind && fread(pack_kind, 1, n, f) == n;
    fclose(f);
    for (u32 i = 0; ok && i < n; i++) ok = pack_kind[i] <= PACK_BOTH;
    return ok;
}
static void packs_save(u64 key, u32 count) {
    char path[1100];
    snprintf(path, sizeof path, "%s.en.packs", discs[0].path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(PACKS_MAGIC, 1, 8, f); fwrite(&key, 8, 1, f); fwrite(&count, 4, 1, f);
    fwrite(pack_kind, 1, count, f);
    fclose(f);
}

static void split_packs(void) {
    static u8 original[2][KCD_SPH_MAX];
    if (!discs[0].en && !discs[1].en) return;
    disc_image_t plain[2] = { discs[0], discs[1] };
    for (int i = 0; i < 2; i++) {
        plain[i].en = NULL;
        if (discs[i].en) plain[i].sectors = discs[i].src_sectors;
    }
    u32 lba, size, o_lba, o_size, o_pack, j_lba, j_size, j_pack[2];
    int count = read_index(&discs[0], "ACE.BPH", "ACE.BPB", KCD_BPH_CODE,
                           pack_index[0], &lba, &size, &pack_lba);
    int o_count = read_index(&discs[1], "ACE.BPH", "ACE.BPB", KCD_BPH_CODE,
                             pack_index[1], &o_lba, &o_size, &o_pack);
    int same = count >= 0 && o_count == count && o_size == size;
    for (int i = 0; i < 2 && same; i++)
        same = read_index(&plain[i], "ACE.BPH", "ACE.BPB", KCD_BPH_CODE,
                          original[i], &j_lba, &j_size, &j_pack[i]) == count;
    if (!same) {
        fprintf(stderr, "english: the discs' ACE.BPH do not match; ACE.BPB is read from "
                        "disc %d only\n", discs[0].number);
        return;
    }
    u32 n[4] = { 0, 0, 0, 0 };
    const u8 *idx[4] = { pack_index[0], pack_index[1], original[0], original[1] };
    u64 key = packs_key(idx, size);
    int cached = packs_load(key, (u32)count);
    for (u32 k = 0; k < (u32)count && !cached; k++) {
        const u8 *e = pack_index[0] + KCD_SPH_HEADER + 8u * k;
        const u8 *o = pack_index[1] + KCD_SPH_HEADER + 8u * k;
        const u8 *j = original[0] + KCD_SPH_HEADER + 8u * k;
        const u8 *p = original[1] + KCD_SPH_HEADER + 8u * k;
        int here = discs[0].en && (rd32(e) != rd32(j) ||
                   !record_same(&discs[0], pack_lba + rd32(e + 4), &plain[0],
                                j_pack[0] + rd32(j + 4), record_sectors(j)));
        int there = discs[1].en && (rd32(o) != rd32(p) ||
                    !record_same(&discs[1], o_pack + rd32(o + 4), &plain[1],
                                 j_pack[1] + rd32(p + 4), record_sectors(p)));
        if (here && there && rd32(e) == rd32(o) &&
            record_same(&discs[0], pack_lba + rd32(e + 4), &discs[1], o_pack + rd32(o + 4),
                        record_sectors(e)))
            here = there = 0;
        pack_kind[k] = here && there ? PACK_BOTH : here ? PACK_ONLY_BOOTED :
                       there ? PACK_ONLY_OTHER : PACK_SAME;
    }
    if (!cached) packs_save(key, (u32)count);
    for (u32 k = 0; k < (u32)count; k++) n[pack_kind[k]]++;
    for (u32 k = 0; k < (u32)count; k++) {
        u8 *o = pack_index[1] + KCD_SPH_HEADER + 8u * k + 4;
        wr32(o, discs[1].base + o_pack + rd32(o) - pack_lba);
    }
    pack_index_lba = lba;
    pack_index_size = size;
    pack_serve();
    printf("english: ACE.BPB holds %u records translated on disc %d only, %u on disc %d only, "
           "%u that follow the disc being played\n", n[PACK_ONLY_BOOTED], discs[0].number,
           n[PACK_ONLY_OTHER], discs[1].number, n[PACK_BOTH]);
}

static int merge_indices(void) {
    static u8 other[KCD_SPH_MAX];
    u32 lba, size, pack, o_lba, o_size, o_pack;
    int count = read_index(&discs[0], "ACE.SPH", "ACE.SPB", KCD_SPH_CODE,
                           merged_index, &lba, &size, &pack);
    int o_count = read_index(&discs[1], "ACE.SPH", "ACE.SPB", KCD_SPH_CODE,
                             other, &o_lba, &o_size, &o_pack);
    if (count < 0 || o_count != count) return 0;

    u32 number = rd32(merged_index + 4), o_number = rd32(other + 4);
    if (number - 1u > 1u || o_number - 1u > 1u || number == o_number)
        return 0;

    u32 moved = 0;
    for (u32 k = 0; k < (u32)count; k++) {
        u8 *e = merged_index + KCD_SPH_HEADER + 8u * k;
        const u8 *o = other + KCD_SPH_HEADER + 8u * k;
        if (rd32(e) & KCD_ON_THIS_DISC) continue;
        if (!(rd32(o) & KCD_ON_THIS_DISC)) continue;
        wr32(e, rd32(o));
        wr32(e + 4, discs[1].base + o_pack + rd32(o + 4) - pack);
        moved++;
    }
    wr32(merged_index + 4, KCD_DISC_EITHER);

    discs[0].number = (int)number;
    discs[1].number = (int)o_number;
    merged_index_lba = lba;
    merged_index_size = size;
    printf("discs %u and %u mounted as one: %u of %d streams read from disc %u\n",
           number, o_number, moved, count, o_number);
    return 1;
}


/* ---- mounting ------------------------------------------------------------- */

int ac3_disc_open(const char *path, int number) {
    image_close(&discs[1]);
    image_close(&discs[0]);
    merged_index_size = 0;
    pack_index_size = 0;
    pack_disc = 0;
    if (!image_open(&discs[0], path)) return 0;
    discs[0].number = number;
    printf("disc %d: %s (%ld sectors)\n", number, path, discs[0].sectors);
    return 1;
}

int ac3_disc_attach(const char *path) {
    if (!discs[0].fp) return 0;
    image_close(&discs[1]);
    merged_index_size = 0;
    pack_index_size = 0;
    pack_disc = 0;
    if (!image_open(&discs[1], path)) return 0;
    discs[1].base = (u32)discs[0].sectors;
    if (!merge_indices()) {
        fprintf(stderr, "ac3: %s is not the other disc of this game; "
                        "running from one disc\n", path);
        image_close(&discs[1]);
        merged_index_size = 0;
        return 0;
    }
    printf("disc %d: %s (%ld sectors, from lba %u)\n", discs[1].number, path,
           discs[1].sectors, discs[1].base);
    split_packs();
    return 1;
}

int ac3_disc_other_english(void) {
    return discs[1].en != NULL;
}

int ac3_disc_pack_split(void) {
    return pack_index_size != 0;
}

int ac3_disc_pack_number(void) {
    return discs[pack_disc].number;
}

int ac3_disc_other_path(const char *path, char *out, size_t n) {
    const char *p = strstr(path, "(Disc ");
    if (!p || (p[6] != '1' && p[6] != '2') || p[7] != ')') return 0;
    if (strlen(path) >= n) return 0;
    strcpy(out, path);
    out[(p - path) + 6] = (p[6] == '1') ? '2' : '1';
    FILE *f = fopen(out, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int ac3_disc_number(void) {
    return merged_index_size ? (int)KCD_DISC_EITHER : discs[0].number;
}

/* ---- following the disc being played -------------------------------------- */

#define KCD_DISC_QUERY      0x8002B504u
#define KCD_SCENE_STATE     0x800BCE60u
#define KCD_SCENE_FIELD     908u
#define KCD_SCENE_DISCS     0x80112750u
#define KCD_OP_JAL_QUERY    0x0C00AD41u
#define KCD_OP_LH_SCENE     0x8463038Cu
#define KCD_OP_ADD_TABLE    0x24422750u
#define KCD_STATE_PTR       0x800BCBA8u
#define KCD_PACK_TABLE      24u

int ac3_disc_pack_follow(int number) {
    if (!pack_index_size) return 0;
    int want = discs[1].number == number ? 1 : discs[0].number == number ? 0 : -1;
    if (want < 0) return 0;
    if (want == pack_disc) return 1;

    u32 count = rd32(pack_merged + 16);
    u32 table = MEM_R32(MEM_R32(KCD_STATE_PTR) + KCD_PACK_TABLE);
    const u8 *e = pack_merged + KCD_SPH_HEADER;
    int known = guest_ptr(table, 8u * count) != NULL;
    for (u32 k = 0; known && k < count; k++)
        known = MEM_R32(table + 8u * k) == rd32(e + 8u * k) &&
                MEM_R32(table + 8u * k + 4) == pack_lba + rd32(e + 8u * k + 4);
    if (!known) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "english: the game's copy of ACE.BPH is not where it was expected; "
                            "the records that differ by disc stay on disc %d\n",
                    discs[pack_disc].number);
        return 0;
    }
    pack_disc = want;
    pack_serve();
    for (u32 k = 0; k < count; k++) {
        if (pack_kind[k] != PACK_BOTH) continue;
        MEM_W32(table + 8u * k, rd32(e + 8u * k));
        MEM_W32(table + 8u * k + 4, pack_lba + rd32(e + 8u * k + 4));
    }
    hle_trace("CdPack", "playing disc %d", number);
    return 1;
}

void pack_note_read(u32 lba) {
    if (!pack_index_size) return;
    u32 count = rd32(pack_merged + 16);
    for (u32 k = 0; k < count; k++) {
        const u8 *e = pack_merged + KCD_SPH_HEADER + 8u * k;
        if (lba - (pack_lba + rd32(e + 4)) >= record_sectors(e)) continue;
        if (pack_kind[k] == PACK_ONLY_BOOTED) ac3_disc_pack_follow(discs[0].number);
        if (pack_kind[k] == PACK_ONLY_OTHER) ac3_disc_pack_follow(discs[1].number);
        return;
    }
}

int ac3_disc_scene_query(ctx_t *ctx, u32 target) {
    if (target != KCD_DISC_QUERY || !merged_index_size) return 0;
    u32 ra = ctx->r[31];
    if (ra != 0x800F2EC0u && ra != 0x800F30E8u) return 0;
    if (MEM_R32(ra - 8) != KCD_OP_JAL_QUERY || MEM_R32(ra + 0x1C) != KCD_OP_LH_SCENE ||
        MEM_R32(ra + 0x20) != KCD_OP_ADD_TABLE)
        return 0;
    s32 scene = MEM_R16S(MEM_R32(KCD_SCENE_STATE) + KCD_SCENE_FIELD);
    u32 need = MEM_R8U(KCD_SCENE_DISCS + (u32)scene);
    if (need == 1 || need == 2) ac3_disc_pack_follow((int)need);
    ctx->r[2] = (need == 1 || need == 2) ? need : (u32)discs[0].number;
    return 1;
}

/* ---- ac3_cd_describe_lba: which file and record a sector belongs to (logs) */

typedef struct { u32 lba; u32 index; } pack_ent_t;
static struct {
    int built;
    u32 bpb_lba, bpb_end, bph_lba, bph_end, sph_lba, sph_end, spb_lba;
    u32 bpb2_lba, bpb2_end;
    pack_ent_t bpb[1024], spb[1024], bpb2[1024];
    int nbpb, nspb, nbpb2;
} lbamap;

static int pack_ent_cmp(const void *a, const void *b) {
    u32 x = ((const pack_ent_t *)a)->lba, y = ((const pack_ent_t *)b)->lba;
    return x < y ? -1 : x > y;
}

static int lbamap_index(const u8 *idx, u32 size, u32 pack_lba, pack_ent_t *out, int cap) {
    if (size < KCD_SPH_HEADER || rd32(idx) != KCD_INDEX_ID) return 0;
    u32 count = rd32(idx + 16);
    if (KCD_SPH_HEADER + 8u * count > size || count > (u32)cap) return 0;
    for (u32 k = 0; k < count; k++) {
        out[k].lba = pack_lba + rd32(idx + KCD_SPH_HEADER + 8u * k + 4);
        out[k].index = k;
    }
    qsort(out, count, sizeof *out, pack_ent_cmp);
    return (int)count;
}

static void lbamap_build(void) {
    static u8 buf[KCD_SPH_MAX];
    lbamap.built = 1;
    const disc_image_t *d = &discs[0];
    if (!d->fp) return;
    u32 size, bph_size, sph_size;
    if (iso_find(d, "ACE.BPB", &lbamap.bpb_lba, &size))
        lbamap.bpb_end = lbamap.bpb_lba + (size + USER_SIZE - 1) / USER_SIZE;
    iso_find(d, "ACE.SPB", &lbamap.spb_lba, &size);
    if (iso_find(d, "ACE.BPH", &lbamap.bph_lba, &bph_size) && bph_size <= sizeof buf) {
        lbamap.bph_end = lbamap.bph_lba + (bph_size + USER_SIZE - 1) / USER_SIZE;
        int ok = 1;
        for (u32 s = 0; s * USER_SIZE < bph_size && ok; s++)
            ok = image_read_user(d, lbamap.bph_lba + s, buf + s * USER_SIZE);
        if (ok) lbamap.nbpb = lbamap_index(buf, bph_size, lbamap.bpb_lba, lbamap.bpb, 1024);
    }
    if (pack_index_size && iso_find(&discs[1], "ACE.BPB", &lbamap.bpb2_lba, &size)) {
        lbamap.bpb2_lba += discs[1].base;
        lbamap.bpb2_end = lbamap.bpb2_lba + (size + USER_SIZE - 1) / USER_SIZE;
        lbamap.nbpb2 = lbamap_index(pack_index[1], pack_index_size, pack_lba, lbamap.bpb2, 1024);
    }
    if (iso_find(d, "ACE.SPH", &lbamap.sph_lba, &sph_size) && sph_size <= sizeof buf) {
        lbamap.sph_end = lbamap.sph_lba + (sph_size + USER_SIZE - 1) / USER_SIZE;
        if (merged_index_size) {
            lbamap.nspb = lbamap_index(merged_index, merged_index_size, lbamap.spb_lba,
                                       lbamap.spb, 1024);
        } else {
            int ok = 1;
            for (u32 s = 0; s * USER_SIZE < sph_size && ok; s++)
                ok = image_read_user(d, lbamap.sph_lba + s, buf + s * USER_SIZE);
            if (ok) lbamap.nspb = lbamap_index(buf, sph_size, lbamap.spb_lba, lbamap.spb, 1024);
        }
    }
}

static int lbamap_find(const pack_ent_t *v, int n, u32 lba) {
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (v[mid].lba <= lba) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

void ac3_cd_describe_lba(u32 lba, char *out, size_t n) {
    if (!lbamap.built) lbamap_build();
    int i;
    if (lba >= lbamap.bpb_lba && lba < lbamap.bpb_end &&
        (i = lbamap_find(lbamap.bpb, lbamap.nbpb, lba)) >= 0) {
        snprintf(out, n, "BPB#%u+%u", lbamap.bpb[i].index, lba - lbamap.bpb[i].lba);
    } else if (lba >= lbamap.bpb2_lba && lba < lbamap.bpb2_end &&
               (i = lbamap_find(lbamap.bpb2, lbamap.nbpb2, lba)) >= 0) {
        snprintf(out, n, "BPB#%u+%u", lbamap.bpb2[i].index, lba - lbamap.bpb2[i].lba);
    } else if (lba >= lbamap.bph_lba && lba < lbamap.bph_end) {
        snprintf(out, n, "ACE.BPH+%u", lba - lbamap.bph_lba);
    } else if (lba >= lbamap.sph_lba && lba < lbamap.sph_end) {
        snprintf(out, n, "ACE.SPH+%u", lba - lbamap.sph_lba);
    } else if (lbamap.spb_lba && lba >= lbamap.spb_lba &&
               (i = lbamap_find(lbamap.spb, lbamap.nspb, lba)) >= 0) {
        snprintf(out, n, "SPB#%u+%u", lbamap.spb[i].index, lba - lbamap.spb[i].lba);
    } else {
        snprintf(out, n, "lba");
    }
}

void discs_machine_save(FILE *f) {
    for (int i = 0; i < 2; i++) {
        ac3_ms_u32(f, discs[i].fp ? 1u : 0u);
        ac3_ms_u32(f, (u32)discs[i].sectors);
        ac3_ms_u32(f, discs[i].base);
        ac3_ms_u32(f, (u32)discs[i].number);
    }
    ac3_ms_u32(f, merged_index_lba);
    ac3_ms_u32(f, merged_index_size);
}
