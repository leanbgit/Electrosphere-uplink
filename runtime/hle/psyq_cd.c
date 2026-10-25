#include "hle.h"
#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include "ac3_oracle.h"
#include "xa_adpcm.h"
#include "ac3_english.h"
#include "ac3_settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RAW_SECTOR   2352
#define SYNC_SIZE    12
#define USER_OFFSET  24
#define USER_SIZE    2048
#define WHOLE_SIZE   2340

#define CD_MODE_SIZE0 0x10
#define CD_MODE_SIZE1 0x20
#define CD_MODE_RT    0x40

enum {
    C_Nop = 0x01, C_Setloc = 0x02, C_Play = 0x03, C_Forward = 0x04,
    C_Backward = 0x05, C_ReadN = 0x06, C_Standby = 0x07, C_Stop = 0x08,
    C_Pause = 0x09, C_Mute = 0x0B, C_Demute = 0x0C, C_Setfilter = 0x0D,
    C_Setmode = 0x0E, C_Getparam = 0x0F, C_GetlocL = 0x10, C_GetlocP = 0x11,
    C_GetTN = 0x13, C_GetTD = 0x14, C_SeekL = 0x15, C_SeekP = 0x16,
    C_ReadS = 0x1B,
};

#define EV_CLASS_CD   0xF0000003u
#define EV_SPEC_COMPLETE 0x0020u
#define EV_SPEC_READY    0x0040u

extern void ac3_deliver_event(ctx_t *ctx, u32 cls, u32 spec);

static int streaming;
static u32   cur_lba;
static u32   cd_mode;

typedef struct {
    FILE *fp;
    long  sectors;
    u32   base;
    int   number;
    ac3_en_overlay_t *en;
    long  src_sectors;
} disc_image_t;

static disc_image_t discs[2];

#define KCD_SPH_MAX (4u * 2048u)
static u8  merged_index[KCD_SPH_MAX];
static u32 merged_index_lba, merged_index_size;

enum { PACK_SAME, PACK_ONLY_BOOTED, PACK_ONLY_OTHER, PACK_BOTH };
static u8  pack_index[2][KCD_SPH_MAX];
static u8  pack_merged[KCD_SPH_MAX];
static u8  pack_kind[KCD_SPH_MAX / 8];
static u32 pack_index_lba, pack_index_size, pack_lba;
static int pack_disc;

static int filter_active;
static u8  filter_file, filter_chan;

static u8  consumed_chan;
static int consumed_known;

#define SUB_FILE   4
#define SUB_CHAN   5
#define SUB_MODE   6
#define SUBMODE_AUDIO 0x04
#define SUBMODE_DATA  0x08
#define CD_MODE_SF    0x08

static int   sector_credit;
int ac3_cd_rate;

static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f);

static int image_read(const disc_image_t *d, u32 rel, u8 *out) {
    if (!d->fp || (long)rel >= d->sectors) return 0;
    u32 src = rel;
    if (d->en) {
        const u8 *lit = ac3_en_overlay_sector(d->en, rel, &src);
        if (lit) {
            memcpy(out, lit + SYNC_SIZE, WHOLE_SIZE);
            return 1;
        }
        if ((long)src >= d->src_sectors) return 0;
    }
    if (fseek(d->fp, (long)src * RAW_SECTOR + SYNC_SIZE, SEEK_SET) != 0)
        return 0;
    if (fread(out, 1, WHOLE_SIZE, d->fp) != WHOLE_SIZE) return 0;
    if (src != rel) lba_to_msf(rel, &out[0], &out[1], &out[2]);
    return 1;
}

static const disc_image_t *image_for(u32 lba) {
    for (int i = 1; i >= 0; i--) {
        const disc_image_t *d = &discs[i];
        if (d->fp && lba >= d->base && (long)(lba - d->base) < d->sectors)
            return d;
    }
    return NULL;
}

static void index_sector(u8 *out, u32 lba, const u8 *index, u32 index_lba, u32 index_size) {
    if (!index_size || lba < index_lba || lba - index_lba >= KCD_SPH_MAX / USER_SIZE) return;
    u32 off = (lba - index_lba) * USER_SIZE;
    if (off < index_size) {
        u32 n = index_size - off;
        memcpy(out + 12, index + off, n < USER_SIZE ? n : USER_SIZE);
    }
}

static int read_sector_whole(u32 lba, u8 *out) {
    const disc_image_t *d = image_for(lba);
    if (!d || !image_read(d, lba - d->base, out)) return 0;

    if (d->base) lba_to_msf(lba, &out[0], &out[1], &out[2]);

    index_sector(out, lba, merged_index, merged_index_lba, merged_index_size);
    index_sector(out, lba, pack_merged, pack_index_lba, pack_index_size);
    return 1;
}

static u32 delivery_offset(void) {
    if (cd_mode & CD_MODE_SIZE1) return 0;
    if (cd_mode & CD_MODE_SIZE0) return 4;
    return 12;
}

static int read_sector(u32 lba, u8 *out) {
    u8 whole[WHOLE_SIZE];
    if (!read_sector_whole(lba, whole)) return 0;
    memcpy(out, whole + 12, USER_SIZE);
    return 1;
}

static u8  fifo[WHOLE_SIZE];
static u32 fifo_lba;
static int fifo_valid;
static u32 fifo_pos;

static int fifo_step(void) {
    if (!read_sector_whole(cur_lba, fifo)) return -1;
    fifo_lba = cur_lba;
    cur_lba++;

    {
        u8 sub = fifo[SUB_MODE];
        ac3_probe_sector(fifo_lba, fifo[SUB_FILE], fifo[SUB_CHAN], sub,
                         (sub & SUBMODE_DATA) != 0);
        if (!(sub & SUBMODE_DATA)) {
            ac3_xa_sector(fifo);
            return 0;
        }
    }
    fifo_valid = 1;
    fifo_pos = delivery_offset();
    return 1;
}

static int fifo_fill(void) {
    for (int tries = 0; tries < 4096; tries++) {
        int r = fifo_step();
        if (r < 0) return 0;
        if (r > 0) return 1;
    }
    return 0;
}

static int image_read_user(const disc_image_t *d, u32 rel, u8 *out) {
    u8 whole[WHOLE_SIZE];
    if (!image_read(d, rel, whole)) return 0;
    memcpy(out, whole + 12, USER_SIZE);
    return 1;
}

static int iso_find(const disc_image_t *d, const char *want,
                    u32 *out_lba, u32 *out_size) {
    u8 sec[USER_SIZE];
    if (!image_read_user(d, 16, sec)) return 0;
    if (memcmp(sec + 1, "CD001", 5) != 0) return 0;

    u32 root_lba, root_size;
    memcpy(&root_lba, sec + 156 + 2, 4);
    memcpy(&root_size, sec + 156 + 10, 4);

    u32 sectors = (root_size + USER_SIZE - 1) / USER_SIZE;
    for (u32 s = 0; s < sectors; s++) {
        if (!image_read_user(d, root_lba + s, sec)) return 0;
        u32 off = 0;
        while (off < USER_SIZE) {
            u8 len = sec[off];
            if (len == 0) break;
            if (off + len > USER_SIZE) break;
            u32 elba, esize;
            memcpy(&elba, sec + off + 2, 4);
            memcpy(&esize, sec + off + 10, 4);
            u8 namelen = sec[off + 32];
            const char *nm = (const char *)(sec + off + 33);
            u32 n = namelen;
            for (u32 i = 0; i < namelen; i++)
                if (nm[i] == ';') { n = i; break; }
            if (n == strlen(want)) {
                u32 i = 0;
                for (; i < n; i++) {
                    char a = nm[i], b = want[i];
                    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                    if (a != b) break;
                }
                if (i == n) { *out_lba = elba; *out_size = esize; return 1; }
            }
            off += len;
        }
    }
    return 0;
}

static u8 to_bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 from_bcd(u8 v) { return (u32)((v >> 4) * 10 + (v & 15)); }

static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f) {
    u32 t = lba + 150;
    *m = to_bcd(t / (75 * 60));
    *s = to_bcd((t / 75) % 60);
    *f = to_bcd(t % 75);
}

static s32 msf_to_lba_signed(u8 m, u8 s, u8 f) {
    return (s32)(((from_bcd(m) * 60 + from_bcd(s)) * 75 + from_bcd(f))) - 150;
}

static u32 msf_to_lba(u8 m, u8 s, u8 f) {
    s32 t = msf_to_lba_signed(m, s, f);
    return t > 0 ? (u32)t : 0u;
}

#define KCD_INDEX_ID     0x45334341u
#define KCD_SPH_CODE     0x19990427u
#define KCD_BPH_CODE     0x00000427u
#define KCD_DISC_EITHER  3u
#define KCD_ON_THIS_DISC 0x80000000u
#define KCD_SPH_HEADER   20u

static u32 rd32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void wr32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 drive_sectors(void) {
    if (discs[1].fp) return discs[1].base + (u32)discs[1].sectors;
    return (u32)discs[0].sectors;
}

static void image_close(disc_image_t *d) {
    if (d->fp) fclose(d->fp);
    ac3_en_overlay_free(d->en);
    memset(d, 0, sizeof *d);
}

static int image_volume_disc(const disc_image_t *d) {
    u8 pvd[USER_SIZE];
    if (!image_read_user(d, 16, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    char id[33];
    memcpy(id, pvd + 40, 32);
    id[32] = 0;
    for (int i = 31; i >= 0 && (id[i] == ' ' || !id[i]); i--) id[i] = 0;
    size_t n = strlen(id);
    if (n < 7 || strncmp(id, "ACE3ES", 6) != 0) return 0;
    return id[n - 1] == '1' ? 1 : id[n - 1] == '2' ? 2 : 0;
}

static void image_english(disc_image_t *d, const char *path) {
    int disc = image_volume_disc(d);
    if (!disc) {
        fprintf(stderr, "english: %s is not an Ace Combat 3 disc; left as it is\n", path);
        return;
    }
    char patch[1024];
    if (!ac3_en_find_patch(disc, ac3_lang_patch_dir(), patch, sizeof patch)) {
        fprintf(stderr, "english: no Team NEMO patch for disc %d in %s%sgame/translation or "
                        "[ProjectNEMO]*; disc %d stays Japanese\n", disc,
                ac3_lang_patch_dir() ? ac3_lang_patch_dir() : "",
                ac3_lang_patch_dir() ? ", " : "", disc);
        return;
    }
    int cached = 0;
    ac3_en_progress_disc = disc;
    ac3_en_overlay_t *o = ac3_en_overlay_open(path, patch, &cached);
    if (!o) {
        fprintf(stderr, "english: disc %d stays Japanese\n", disc);
        return;
    }
    d->en = o;
    d->src_sectors = d->sectors;
    d->sectors = (long)ac3_en_overlay_sectors(o);
    printf("english: disc %d from %s (%u new sectors%s)\n", disc, patch,
           ac3_en_overlay_literals(o), cached ? ", cached" : "");
}

static int image_open(disc_image_t *d, const char *path) {
    memset(d, 0, sizeof *d);
    d->fp = fopen(path, "rb");
    if (!d->fp) {
        fprintf(stderr, "ac3: cannot open disc image %s\n", path);
        return 0;
    }
    setvbuf(d->fp, NULL, _IOFBF, 1 << 20);
    fseek(d->fp, 0, SEEK_END);
    d->sectors = ftell(d->fp) / RAW_SECTOR;
    if (ac3_lang_english()) image_english(d, path);
    return 1;
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
    for (u32 k = 0; k < (u32)count; k++) {
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
        n[pack_kind[k]]++;
    }
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

static void pack_note_read(u32 lba) {
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

void psyq_CdInit(ctx_t *ctx) {
    if (!discs[0].fp) hle_warn("CdInit", "no disc image mounted");
    cur_lba = 0;
    cd_mode = 0;
    RET(discs[0].fp ? 1 : 0);
}

#define STAT_ERROR   0x01
#define STAT_MOTOR   0x02
#define STAT_SHELL   0x10
#define STAT_READING 0x20
#define STAT_SEEKING 0x40
#define STAT_PLAYING 0x80

static u8 drive_status(void) {
    u8 s = discs[0].fp ? STAT_MOTOR : (STAT_SHELL | STAT_ERROR);
    if (discs[0].fp && streaming) s |= STAT_READING;
    return s;
}

static void write_status(u32 result) {
    if (!result) return;
    u8 *r = (u8 *)guest_ptr(result, 8);
    if (!r) return;
    memset(r, 0, 8);
    r[0] = drive_status();
}

static u32 sync_callback_fn;
static u32 ready_callback_fn;

#define CdlDataReady 1
#define CdlComplete  2
#define CdlDataEnd   3
#define CdlAcknowledge 4

static void invoke_cb(ctx_t *ctx, u32 fn, u32 intr, u32 result) {
    if (!fn) return;
    u32 sa0 = ctx->r[4], sa1 = ctx->r[5];
    ctx->r[4] = intr;
    ctx->r[5] = result;
    ac3_call_indirect(ctx, fn);
    ctx->r[4] = sa0;
    ctx->r[5] = sa1;
}

void psyq_CdSyncCallback(ctx_t *ctx) {
    u32 prev = sync_callback_fn;
    sync_callback_fn = A0;
    hle_trace("CdSyncCallback", "fn=0x%08X", A0);
    RET(prev);
}

void psyq_CdReadyCallback(ctx_t *ctx) {
    u32 prev = ready_callback_fn;
    ready_callback_fn = A0;
    hle_trace("CdReadyCallback", "fn=0x%08X", A0);
    RET(prev);
}

void psyq_CD_init(ctx_t *ctx)     { RET(0); }
void psyq_CD_initintr(ctx_t *ctx) { RET(0); }
void psyq_CD_initvol(ctx_t *ctx)  { RET(0); }
void psyq_CD_flush(ctx_t *ctx)    { RET(0); }

void psyq_CD_sync(ctx_t *ctx) {
    write_status(A1);
    RET(2);
}

void psyq_CD_ready(ctx_t *ctx) {
    write_status(A1);
    RET(streaming ? 1 : 0);
}
void psyq_CD_datasync(ctx_t *ctx) { RET(0); }
void psyq_CD_vol(ctx_t *ctx)      { RET(0); }
void psyq_CD_set_test_parmnum(ctx_t *ctx) { RET(0); }
void psyq_CD_getsector(ctx_t *ctx) {
    hle_trace("CD_getsector", "a0=0x%08X a1=0x%08X", A0, A1);
    RET(0);
}
void psyq_CD_cw(ctx_t *ctx) {
    hle_trace("CD_cw", "com=0x%02X a1=0x%08X a2=0x%08X a3=0x%08X",
              A0 & 0xFF, A1, A2, A3);
    RET(0);
}
void psyq_CdMix(ctx_t *ctx)       { RET(1); }
void psyq_DsMix(ctx_t *ctx)       { RET(1); }
void psyq_CdSync(ctx_t *ctx) {
    write_status(A1);
    RET(2);
}

void psyq_CdPosToInt(ctx_t *ctx) {
    u8 *loc = (u8 *)guest_ptr(A0, 4);
    if (!loc) { RET(0); return; }
    RET(msf_to_lba_signed(loc[0], loc[1], loc[2]));
}

void psyq_DsPosToInt(ctx_t *ctx) { psyq_CdPosToInt(ctx); }

void psyq_CdIntToPos(ctx_t *ctx) {
    u8 *loc = (u8 *)guest_ptr(A1, 4);
    if (loc) {
        lba_to_msf(A0, &loc[0], &loc[1], &loc[2]);
        loc[3] = 0;
    }
    RET(A1);
}

void psyq_CdSearchFile(ctx_t *ctx) {
    u8 *fp = (u8 *)guest_ptr(A0, 24);
    char name[64];
    u32 na = A1;
    int n = 0;
    while (n < (int)sizeof name - 1) {
        u8 c = (u8)MEM_R8U(na + (u32)n);
        if (c == 0) break;
        name[n++] = (char)c;
    }
    name[n] = 0;

    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char clean[64];
    int m = 0;
    for (const char *p = base; *p && *p != ';' && m < 63; p++) clean[m++] = *p;
    clean[m] = 0;

    u32 lba = 0, size = 0;
    if (!fp || !iso_find(&discs[0], clean, &lba, &size)) {
        hle_warn("CdSearchFile", "not found: %s", clean);
        RET(0);
        return;
    }
    lba_to_msf(lba, &fp[0], &fp[1], &fp[2]);
    fp[3] = 0;
    memcpy(fp + 4, &size, 4);
    memset(fp + 8, 0, 16);
    memcpy(fp + 8, clean, (size_t)(m < 15 ? m : 15));
    hle_trace("CdSearchFile", "%s -> lba %u size %u", clean, lba, size);
    RET(A0);
}

static void cd_command(ctx_t *ctx, u32 com, u32 param, u32 result);

void psyq_CdControl(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, A2);
    RET(1);
}

void psyq_CdControlB(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, A2);
    RET(1);
}

void psyq_CdControlF(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, 0);
    invoke_cb(ctx, sync_callback_fn, CdlComplete, 0);
    RET(1);
}

static void cd_command(ctx_t *ctx, u32 com, u32 param, u32 result) {
    switch (com) {
    case C_Setloc: {
        u8 *loc = (u8 *)guest_ptr(param, 4);
        if (loc) {
            u32 lba = msf_to_lba(loc[0], loc[1], loc[2]);
            if (lba != cur_lba) {
                cur_lba = lba;
                fifo_valid = 0;
                consumed_known = 0;
                if (sector_credit > 0) sector_credit = 0;
            }
        }
        break;
    }
    case C_Setmode:
        cd_mode = param ? MEM_R8U(param) : 0;
        ac3_xa_set_enabled((cd_mode & CD_MODE_RT) != 0);
        break;
    case C_GetTN:
        if (result) { MEM_W8(result, 0x01); MEM_W8(result + 1, 0x01); }
        break;
    case C_GetTD:
        if (result) {
            u8 m, s, f;
            lba_to_msf(drive_sectors(), &m, &s, &f);
            MEM_W8(result, m); MEM_W8(result + 1, s); MEM_W8(result + 2, f);
        }
        break;
    case C_GetlocL:
    case C_GetlocP:
        if (result) {
            u8 m, s, f;
            lba_to_msf(cur_lba, &m, &s, &f);
            MEM_W8(result, m); MEM_W8(result + 1, s); MEM_W8(result + 2, f);
            MEM_W8(result + 3, 1);
        }
        break;
    case C_ReadN:
    case C_ReadS:
        pack_note_read(cur_lba);
        streaming = 1;
        ac3_probe_stream_begin(ctx, com, cur_lba, filter_file, filter_chan);
        break;
    case C_Pause:
    case C_Stop:
    case C_Standby:
        streaming = 0;
        ac3_xa_stop();
        ac3_probe_stream_end(ctx, cur_lba);
        break;
    case C_Mute:
        ac3_xa_set_muted(1);
        break;
    case C_Demute:
        ac3_xa_set_muted(0);
        break;
    case C_Setfilter: {
        const u8 *f = (const u8 *)guest_ptr(param, 2);
        if (f) {
            filter_file = f[0];
            filter_chan = f[1];
            filter_active = 1;
            ac3_xa_set_filter(1, filter_file, filter_chan);
            hle_trace("CdControl", "Setfilter file=%u chan=%u",
                      filter_file, filter_chan);
        }
        break;
    }
    case C_SeekL:
    case C_SeekP:
    case C_Nop:
        break;
    default:
        hle_warn("CdControl", "unhandled command 0x%02X", com);
        break;
    }
    if (com != C_GetTN && com != C_GetTD && com != C_GetlocL && com != C_GetlocP)
        write_status(result);
    {
        extern u32 ac3_cur_func;
        hle_trace("CdCommand", "com=0x%02X param=0x%08X -> lba %u  from func 0x%08X",
                  com, param, cur_lba, ac3_cur_func);
    }
    ac3_deliver_event(ctx, EV_CLASS_CD, EV_SPEC_COMPLETE);
}

void psyq_CdRead(ctx_t *ctx) {
    u32 sectors = A0, dst = A1;
    cd_mode = A2;
    u8 buf[USER_SIZE];
    u32 ok = 0;
    pack_note_read(cur_lba);
    for (u32 i = 0; i < sectors; i++) {
        if (!read_sector(cur_lba + i, buf)) {
            hle_warn("CdRead", "read failed at lba %u", cur_lba + i);
            break;
        }
        u8 *g = (u8 *)guest_ptr(dst + i * USER_SIZE, USER_SIZE);
        if (!g) { hle_warn("CdRead", "bad destination 0x%08X", dst); break; }
        memcpy(g, buf, USER_SIZE);
        ac3_watch_bulk(dst + i * USER_SIZE, USER_SIZE, "CdRead");
        ac3_oracle_cd(cur_lba + i, dst + i * USER_SIZE, USER_SIZE);
        ok++;
    }
    hle_trace("CdRead", "lba %u, %u sectors -> 0x%08X, first word 0x%08X",
              cur_lba, ok, dst, ok ? MEM_R32(dst) : 0);
    cur_lba += ok;
    ac3_deliver_event(ctx, EV_CLASS_CD, EV_SPEC_COMPLETE);
    RET(ok == sectors);
}

void psyq_CdReadSync(ctx_t *ctx) {
    if (A1) MEM_W8(A1, 0x02);
    RET(0);
}

void psyq_CdReadBreak(ctx_t *ctx) { RET(0); }

static u32 data_callback_fn;

void psyq_CdDataCallback(ctx_t *ctx) {
    u32 prev = data_callback_fn;
    data_callback_fn = A0;
    hle_trace("CdDataCallback", "fn=0x%08X", A0);
    RET(prev);
}
void psyq_DsDataCallback(ctx_t *ctx) { psyq_CdDataCallback(ctx); }

void psyq_CdGetSector(ctx_t *ctx) {
    u32 dst = A0, words = A1;
    u32 bytes = words * 4;

    if (!fifo_valid && !fifo_fill()) {
        hle_warn("CdGetSector", "read failed at lba %u", cur_lba);
        RET(0);
        return;
    }
    u8 *g = (u8 *)guest_ptr(dst, bytes);
    if (!g) { hle_warn("CdGetSector", "bad destination 0x%08X", dst); RET(0); return; }
    if (fifo_pos >= WHOLE_SIZE) fifo_pos = delivery_offset();
    if (bytes > WHOLE_SIZE - fifo_pos) bytes = WHOLE_SIZE - fifo_pos;
    memcpy(g, fifo + fifo_pos, bytes);
    ac3_watch_bulk(dst, bytes, "CdGetSector");
    const u8 *buf = fifo + fifo_pos;
    if (bytes >= 512) {
        consumed_chan = fifo[SUB_CHAN];
        consumed_known = 1;
        ac3_oracle_cd(fifo_lba, dst, bytes);
    }
    ac3_probe_read(bytes, buf, fifo[SUB_CHAN], fifo_pos);
    fifo_pos += bytes;
    if (fifo_pos >= WHOLE_SIZE) fifo_valid = 0;
    hle_trace("CdGetSector", "lba %u -> 0x%08X (%u bytes) data %02X %02X %02X %02X %02X %02X %02X %02X",
              fifo_lba, dst, bytes, buf[0], buf[1], buf[2], buf[3],
              buf[4], buf[5], buf[6], buf[7]);
    RET(1);
}
void psyq_CdGetSector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }
void psyq_DsGetSector(ctx_t *ctx)  { psyq_CdGetSector(ctx); }
void psyq_DsGetSector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }
void psyq_CD_getsector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }

void psyq_CdReady(ctx_t *ctx) {
    if (streaming && !fifo_valid) fifo_fill();
    write_status(A1);
    RET((streaming && fifo_valid) ? 1 : 0);
}
void psyq_DS_ready(ctx_t *ctx) { psyq_CdReady(ctx); }

void psyq_CdDataSync(ctx_t *ctx) { RET(0); }
void psyq_DsDataSync(ctx_t *ctx) { RET(0); }
void psyq_DS_sync(ctx_t *ctx)    { RET(2); }

void psyq_CdReset(ctx_t *ctx)   { streaming = 0; cur_lba = 0; ac3_xa_reset(); RET(1); }
void psyq_CdFlush(ctx_t *ctx)   { RET(0); }
void psyq_CdLastPos(ctx_t *ctx) { RET(0); }

void psyq_CdComstr(ctx_t *ctx) { RET(0); }
void psyq_CdIntstr(ctx_t *ctx) { RET(0); }

#define CD_MODE_SPEED 0x80
#define FRAMES_PER_SEC 60

static int delivery_rate(void) {
    if (ac3_cd_rate > 0) return ac3_cd_rate;
    return (cd_mode & CD_MODE_SPEED) ? 150 : 75;
}

enum { DEL_END = -1, DEL_FILTERED = 0, DEL_DELIVERED = 1 };

static int deliver_one(ctx_t *ctx) {
    int r = fifo_step();
    if (r < 0) return DEL_END;
    if (r == 0) return DEL_FILTERED;
    invoke_cb(ctx, ready_callback_fn, CdlDataReady, 0);
    if (data_callback_fn) invoke_cb(ctx, data_callback_fn, CdlDataReady, 0);
    return DEL_DELIVERED;
}

static int fifo_ends_record(void) {
    if (consumed_known && fifo[SUB_CHAN] != consumed_chan) return 0;
    const u8 *p = fifo + 12;
    u32 chunks = (u32)p[2] | ((u32)p[3] << 8);
    return p[0] == 1 && chunks != 0 && (u32)p[1] + 1u >= chunks;
}

void ac3_cd_tick(ctx_t *ctx) {
    sector_credit += delivery_rate();
    if (!streaming) {
        if (sector_credit > 0) sector_credit = 0;
        return;
    }
    while (sector_credit >= FRAMES_PER_SEC) {
        int r = deliver_one(ctx);
        if (r == DEL_END) break;
        sector_credit -= FRAMES_PER_SEC;
        if (!streaming) break;
    }
}

void ac3_cd_stream_ahead(ctx_t *ctx) {
    if (!streaming) return;
    for (int i = 0; i < 256; i++) {
        int r = deliver_one(ctx);
        if (r == DEL_END) break;
        sector_credit -= FRAMES_PER_SEC;
        if (!streaming) break;
        if (r == DEL_DELIVERED && fifo_ends_record()) break;
    }
}

void ac3_cd_snapshot(ac3_cd_snap_t *s) {
    if (!s) return;
    s->lba = cur_lba;
    s->mode = cd_mode;
    s->streaming = streaming;
    s->credit = sector_credit;
    s->rate = delivery_rate();
    s->filter_active = filter_active;
    s->filter_file = filter_file;
    s->filter_chan = filter_chan;
    s->fifo_lba = fifo_lba;
    s->fifo_valid = fifo_valid;
}

int ac3_cd_debt_frames(void) {
    int rate = delivery_rate();
    if (sector_credit >= 0 || rate <= 0) return 0;
    return (-sector_credit + rate - 1) / rate;
}

void psyq_CdGetToc(ctx_t *ctx) {
    u32 out = A0;
    if (out) {
        u8 m, s, f;
        lba_to_msf(0, &m, &s, &f);
        MEM_W8(out, 0); MEM_W8(out + 1, m);
        MEM_W8(out + 2, s); MEM_W8(out + 3, f);
    }
    RET(1);
}

void ac3_cd_machine_save(FILE *f) {
    ac3_ms_tag(f, "CD");
    ac3_ms_u32(f, (u32)streaming);
    ac3_ms_u32(f, cur_lba);
    ac3_ms_u32(f, cd_mode);
    ac3_ms_u32(f, (u32)filter_active);
    ac3_ms_u32(f, filter_file);
    ac3_ms_u32(f, filter_chan);
    ac3_ms_u32(f, consumed_chan);
    ac3_ms_u32(f, (u32)consumed_known);
    ac3_ms_u32(f, (u32)sector_credit);
    ac3_ms_u32(f, (u32)ac3_cd_rate);
    ac3_ms_bytes(f, fifo, WHOLE_SIZE);
    ac3_ms_u32(f, fifo_lba);
    ac3_ms_u32(f, (u32)fifo_valid);
    ac3_ms_u32(f, fifo_pos);
    ac3_ms_u32(f, sync_callback_fn);
    ac3_ms_u32(f, ready_callback_fn);
    ac3_ms_u32(f, data_callback_fn);
    for (int i = 0; i < 2; i++) {
        ac3_ms_u32(f, discs[i].fp ? 1u : 0u);
        ac3_ms_u32(f, (u32)discs[i].sectors);
        ac3_ms_u32(f, discs[i].base);
        ac3_ms_u32(f, (u32)discs[i].number);
    }
    ac3_ms_u32(f, merged_index_lba);
    ac3_ms_u32(f, merged_index_size);
}
