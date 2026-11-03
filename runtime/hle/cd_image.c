/*
 * cd_image.c -- one disc image file: raw 2352-byte sectors (a .bin), its
 * ISO 9660 directory, and the English version laid over it (Project NEMO's
 * patch, built by ac3_english.c: the patched sectors come from the overlay,
 * all others from the file).
 */
#include "cd_internal.h"
#include "ac3_settings.h"
#include <string.h>
#include <stdlib.h>

/* ---- sectors -------------------------------------------------------------- */

/* sector rel of the image, whole (header, subheader, data) */
int image_read(const disc_image_t *d, u32 rel, u8 *out) {
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

int image_read_user(const disc_image_t *d, u32 rel, u8 *out) {
    u8 whole[WHOLE_SIZE];
    if (!image_read(d, rel, whole)) return 0;
    memcpy(out, whole + 12, USER_SIZE);
    return 1;
}

/* a file in the root directory (names compared without case or ";1") */
int iso_find(const disc_image_t *d, const char *want,
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

/* ---- positions: minutes, seconds, frames (BCD) from the disc's start ------- */

static u8 to_bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 from_bcd(u8 v) { return (u32)((v >> 4) * 10 + (v & 15)); }

void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f) {
    u32 t = lba + 150;
    *m = to_bcd(t / (75 * 60));
    *s = to_bcd((t / 75) % 60);
    *f = to_bcd(t % 75);
}

s32 msf_to_lba_signed(u8 m, u8 s, u8 f) {
    return (s32)(((from_bcd(m) * 60 + from_bcd(s)) * 75 + from_bcd(f))) - 150;
}

u32 msf_to_lba(u8 m, u8 s, u8 f) {
    s32 t = msf_to_lba_signed(m, s, f);
    return t > 0 ? (u32)t : 0u;
}

/* ---- opening ---------------------------------------------------------------- */

void image_close(disc_image_t *d) {
    if (d->fp) fclose(d->fp);
    ac3_en_overlay_free(d->en);
    memset(d, 0, sizeof *d);
}

/* 1 or 2 from the volume name (ACE3ES...1 or 2), 0: not this game's disc */
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

/* The English version of this disc, if its patch is found (otherwise the disc
 * stays Japanese). */
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

int image_open(disc_image_t *d, const char *path) {
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
