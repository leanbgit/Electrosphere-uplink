#include "ac3_english.h"

/* The browser build cannot map a 700 MB disc image into its 32-bit address
 * space, so there the image is read piece by piece instead (AC3_EN_READ).
 * Native builds keep the memory map. */
#if defined(__EMSCRIPTEN__) && !defined(AC3_EN_READ)
#define AC3_EN_READ 1
#endif
#include "vcdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define RAW   2352u
#define PROBE 1024u
#define LIT_FLAG 0x80000000u
#define NO_HINT  0xFFFFFFFFu

struct ac3_en_overlay {
    u32 sectors;
    u32 *map;
    u8  *lit;
    u32  nlit, caplit;
};

typedef struct {
    const u8 *base;
    size_t size;
#ifdef AC3_EN_READ
    FILE *fp;
    u8 *win;
    size_t win_cap;
#endif
#ifdef _WIN32
    HANDLE file, mapping;
#else
    int fd;
#endif
} image_map_t;

static int map_open(image_map_t *m, const char *path) {
    memset(m, 0, sizeof *m);
#if defined(AC3_EN_READ)
    m->fp = fopen(path, "rb");
    if (!m->fp) return 0;
    if (fseek64(m->fp, 0, SEEK_END) != 0) { fclose(m->fp); return 0; }
    long long n = (long long)ftell64(m->fp);
    if (n <= 0) { fclose(m->fp); return 0; }
    m->size = (size_t)n;
#elif defined(_WIN32)
    m->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (m->file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(m->file, &sz) || sz.QuadPart <= 0) { CloseHandle(m->file); return 0; }
    m->size = (size_t)sz.QuadPart;
    m->mapping = CreateFileMappingA(m->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m->mapping) { CloseHandle(m->file); return 0; }
    m->base = (const u8 *)MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!m->base) { CloseHandle(m->mapping); CloseHandle(m->file); return 0; }
#else
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) return 0;
    struct stat st;
    if (fstat(m->fd, &st) != 0 || st.st_size <= 0) { close(m->fd); return 0; }
    m->size = (size_t)st.st_size;
    void *p = mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (p == MAP_FAILED) { close(m->fd); return 0; }
    m->base = p;
#endif
    return 1;
}

static void map_close(image_map_t *m) {
#if defined(AC3_EN_READ)
    if (m->fp) fclose(m->fp);
    free(m->win);
#elif defined(_WIN32)
    if (m->base) UnmapViewOfFile(m->base);
    if (m->mapping) CloseHandle(m->mapping);
    if (m->file && m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
#else
    if (m->base) munmap((void *)m->base, m->size);
    if (m->fd > 0) close(m->fd);
#endif
    memset(m, 0, sizeof *m);
}

/* Bytes [pos, pos+len) of the image: a pointer into the map, or (read mode)
 * into a buffer that stays valid until the next call. */
static const u8 *map_range(image_map_t *m, u64 pos, u64 len) {
#ifdef AC3_EN_READ
    if (len > m->win_cap) {
        u8 *nw = realloc(m->win, (size_t)len ? (size_t)len : 1);
        if (!nw) return NULL;
        m->win = nw;
        m->win_cap = (size_t)len;
    }
    if (fseek64(m->fp, (long long)pos, SEEK_SET) != 0 ||
        fread(m->win, 1, (size_t)len, m->fp) != (size_t)len) return NULL;
    return m->win;
#else
    return m->base + pos;
#endif
}

/* One raw sector of the image; tmp must hold RAW bytes (used in read mode). */
static const u8 *map_sector(const image_map_t *m, u32 s, u8 *tmp) {
#ifdef AC3_EN_READ
    if (fseek64(m->fp, (long long)s * RAW, SEEK_SET) != 0 ||
        fread(tmp, 1, RAW, m->fp) != RAW) return NULL;
    return tmp;
#else
    (void)tmp;
    return m->base + (size_t)s * RAW;
#endif
}

static u8 *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static int has_token(const char *s, const char *tok) {
    size_t n = strlen(tok);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tok[i]) i++;
        if (i == n && !isdigit((unsigned char)s[n])) return 1;
    }
    return 0;
}

static int patch_disc(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (int d = 1; d <= 2; d++) {
        char t1[8], t2[8];
        snprintf(t1, sizeof t1, "cd%d", d);
        snprintf(t2, sizeof t2, "disc %d", d);
        if (has_token(base, t1) || has_token(base, t2)) return d;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char head[640];
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = 0;
    for (size_t i = 0; i < n; i++) if (!head[i]) head[i] = ' ';
    for (int d = 1; d <= 2; d++) {
        char t[8];
        snprintf(t, sizeof t, "cd%d", d);
        if (has_token(head, t)) return d;
    }
    return 0;
}

static int ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

static int starts_with(const char *s, const char *pre) {
    return strncmp(s, pre, strlen(pre)) == 0;
}

typedef int (*entry_fn)(const char *full, int is_dir, void *user);

static int each_entry(const char *dir, entry_fn fn, void *user) {
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int r = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        r = fn(full, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
    } while (!r && FindNextFileA(h, &fd));
    FindClose(h);
    return r;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    int r = 0;
    struct dirent *e;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        r = fn(full, is_dir, user);
    }
    closedir(d);
    return r;
#endif
}

typedef struct {
    int disc, depth;
    char *out;
    size_t n;
} find_t;

static int find_in(const char *full, int is_dir, void *user) {
    find_t *f = user;
    if (is_dir) {
        if (f->depth <= 0) return 0;
        find_t sub = *f;
        sub.depth--;
        return each_entry(full, find_in, &sub);
    }
    if (!ends_with_ci(full, ".xdelta") || patch_disc(full) != f->disc) return 0;
    snprintf(f->out, f->n, "%s", full);
    return 1;
}

static int find_nemo(const char *full, int is_dir, void *user) {
    const char *base = full;
    for (const char *p = full; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!is_dir || !starts_with(base, "[ProjectNEMO]")) return 0;
    return each_entry(full, find_in, user);
}

int ac3_en_find_patch(int disc, const char *dir, char *out, size_t n) {
    find_t f = { disc, 2, out, n };
    if (dir && *dir) {
        if (ends_with_ci(dir, ".xdelta") && patch_disc(dir) == disc) {
            snprintf(out, n, "%s", dir);
            return 1;
        }
        if (each_entry(dir, find_in, &f)) return 1;
    }
    if (each_entry("game/translation", find_in, &f)) return 1;
    return each_entry(".", find_nemo, &f);
}

typedef struct {
    u32 *hint;
    u32 sectors;
} hint_ctx_t;

static void on_copy(void *user, u64 toff, u32 size, u64 soff) {
    hint_ctx_t *h = user;
    u64 end = toff + size;
    u64 first = toff > PROBE ? (toff - PROBE + RAW - 1) / RAW : 0;
    for (u64 k = first; k < h->sectors; k++) {
        u64 probe = k * RAW + PROBE;
        if (probe >= end) break;
        if (probe < toff) continue;
        u64 s = soff + (probe - toff);
        if (s >= PROBE && (s - PROBE) % RAW == 0) h->hint[k] = (u32)((s - PROBE) / RAW);
    }
}

static void msf_of(u32 lba, u8 *m) {
    u32 t = lba + 150;
    u32 mm = t / 4500, ss = (t / 75) % 60, ff = t % 75;
    m[0] = (u8)(((mm / 10) << 4) | (mm % 10));
    m[1] = (u8)(((ss / 10) << 4) | (ss % 10));
    m[2] = (u8)(((ff / 10) << 4) | (ff % 10));
}

typedef struct {
    ac3_en_overlay_t *o;
    const image_map_t *src;
    u32 src_sectors;
    const u32 *hint;
    u32 last_src;
    int ok;
} classify_t;

static int same_as(const classify_t *c, const u8 *sec, u32 s) {
    if (s >= c->src_sectors) return 0;
    u8 tmp[RAW];
    const u8 *q = map_sector(c->src, s, tmp);
    if (!q) return 0;
    return memcmp(sec, q, 12) == 0 && memcmp(sec + 15, q + 15, RAW - 15) == 0;
}

static void classify(classify_t *c, u32 k, const u8 *sec) {
    ac3_en_overlay_t *o = c->o;
    u8 m[3];
    msf_of(k, m);
    if (memcmp(sec + 12, m, 3) == 0) {
        u32 cand[3] = { k, c->hint[k], c->last_src + 1 };
        for (int i = 0; i < 3; i++) {
            if (cand[i] == NO_HINT || !same_as(c, sec, cand[i])) continue;
            o->map[k] = cand[i];
            c->last_src = cand[i];
            return;
        }
    }
    if (o->nlit == o->caplit) {
        u32 nc = o->caplit ? o->caplit * 2 : 1024;
        u8 *nl = realloc(o->lit, (size_t)nc * RAW);
        if (!nl) { c->ok = 0; return; }
        o->lit = nl;
        o->caplit = nc;
    }
    memcpy(o->lit + (size_t)o->nlit * RAW, sec, RAW);
    o->map[k] = LIT_FLAG | o->nlit++;
    c->last_src = NO_HINT - 1;
}

void (*ac3_en_progress)(int disc, u32 done, u32 total);
int ac3_en_progress_disc;

ac3_en_overlay_t *ac3_en_overlay_build(const char *image_path, const char *patch_path) {
    size_t plen = 0;
    u8 *patch = read_file(patch_path, &plen);
    if (!patch) {
        fprintf(stderr, "english: cannot read %s\n", patch_path);
        return NULL;
    }
    vcd_patch_t vp;
    if (!vcd_parse(patch, plen, &vp)) {
        fprintf(stderr, "english: %s is not an xdelta patch this can read\n", patch_path);
        free(patch);
        return NULL;
    }
    ac3_en_overlay_t *o = NULL;
    image_map_t src;
    u8 *tbuf = NULL;
    u32 *hint = NULL;
    if (vp.target_size % RAW || vp.target_size / RAW > 0x7FFFFFFFu) {
        fprintf(stderr, "english: %s does not make a disc image\n", patch_path);
        goto fail_patch;
    }
    if (!map_open(&src, image_path)) {
        fprintf(stderr, "english: cannot map %s\n", image_path);
        goto fail_patch;
    }
    o = calloc(1, sizeof *o);
    u32 nsec = (u32)(vp.target_size / RAW);
    tbuf = malloc(vp.max_tlen ? vp.max_tlen : 1);
    hint = malloc((size_t)nsec * sizeof *hint);
    if (o) o->map = malloc((size_t)nsec * sizeof *o->map);
    if (!o || !o->map || !tbuf || !hint) goto fail;
    memset(hint, 0xFF, (size_t)nsec * sizeof *hint);
    o->sectors = nsec;
    hint_ctx_t hc = { hint, nsec };
    classify_t cl = { o, &src, (u32)(src.size / RAW), hint, NO_HINT - 1, 1 };
    u8 part[RAW];
    u32 part_fill = 0, next = 0;
    for (int i = 0; i < vp.nwin; i++) {
        const vcd_window_t *w = &vp.win[i];
        if (w->src_pos + w->src_len > src.size) {
            fprintf(stderr, "english: %s is smaller than the disc the patch was made for\n", image_path);
            goto fail;
        }
        const u8 *srcwin = map_range(&src, w->src_pos, w->src_len);
        if (!srcwin) {
            fprintf(stderr, "english: cannot read %s\n", image_path);
            goto fail;
        }
        if (!vcd_apply_window(w, srcwin, tbuf, on_copy, &hc) ||
            (w->has_adler && vcd_adler32(tbuf, w->tlen) != w->adler)) {
            fprintf(stderr, "english: %s does not match the disc image the patch was made for "
                            "(it wants the Redump \"Rev 1\" image)\n", image_path);
            goto fail;
        }
        u32 off = 0;
        if (part_fill) {
            u32 take = RAW - part_fill < w->tlen ? RAW - part_fill : w->tlen;
            memcpy(part + part_fill, tbuf, take);
            part_fill += take;
            off = take;
            if (part_fill == RAW) { classify(&cl, next++, part); part_fill = 0; }
        }
        while (off + RAW <= w->tlen) {
            classify(&cl, next++, tbuf + off);
            off += RAW;
        }
        if (off < w->tlen) {
            memcpy(part, tbuf + off, w->tlen - off);
            part_fill = w->tlen - off;
        }
        if (!cl.ok) goto fail;
        if (ac3_en_progress) ac3_en_progress(ac3_en_progress_disc, (u32)i + 1, (u32)vp.nwin);
    }
    if (next != nsec || part_fill) goto fail;
    free(tbuf);
    free(hint);
    map_close(&src);
    vcd_free(&vp);
    free(patch);
    return o;
fail:
    map_close(&src);
    free(tbuf);
    free(hint);
    ac3_en_overlay_free(o);
    o = NULL;
fail_patch:
    vcd_free(&vp);
    free(patch);
    return NULL;
}

#define CACHE_MAGIC   "AC3ENOV1"
#define CACHE_SAMPLES 64u

typedef struct {
    char magic[8];
    u32 version;
    u32 patch_adler;
    u64 patch_size;
    u64 image_size;
    u32 image_sample;
    u32 sectors;
    u32 nlit;
    u32 body_adler;
} cache_head_t;

static int image_sample(const char *path, u64 *size, u32 *sum) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek64(f, 0, SEEK_END);
    long long n = (long long)ftell64(f);
    if (n < (long long)RAW) { fclose(f); return 0; }
    u64 secs = (u64)n / RAW;
    u32 a = 1;
    u8 sec[RAW];
    for (u32 i = 0; i < CACHE_SAMPLES; i++) {
        u64 lba = i < 32 ? i : secs * (i - 32) / (CACHE_SAMPLES - 32);
        if (lba >= secs) lba = secs - 1;
        fseek64(f, (long long)(lba * RAW), SEEK_SET);
        if (fread(sec, 1, RAW, f) != RAW) { fclose(f); return 0; }
        a = vcd_adler32(sec, RAW) ^ (a * 2654435761u);
    }
    fclose(f);
    *size = (u64)n;
    *sum = a;
    return 1;
}

static int cache_key(const char *image_path, const char *patch_path, cache_head_t *h) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, CACHE_MAGIC, 8);
    h->version = 1;
    size_t plen = 0;
    u8 *p = read_file(patch_path, &plen);
    if (!p) return 0;
    h->patch_size = plen;
    h->patch_adler = vcd_adler32(p, plen);
    free(p);
    return image_sample(image_path, &h->image_size, &h->image_sample);
}

static ac3_en_overlay_t *cache_load(const char *path, const cache_head_t *key) {
    size_t len = 0;
    u8 *b = read_file(path, &len);
    if (!b) return NULL;
    cache_head_t h;
    ac3_en_overlay_t *o = NULL;
    if (len < sizeof h) goto out;
    memcpy(&h, b, sizeof h);
    if (memcmp(h.magic, key->magic, 8) || h.version != key->version ||
        h.patch_adler != key->patch_adler || h.patch_size != key->patch_size ||
        h.image_size != key->image_size || h.image_sample != key->image_sample)
        goto out;
    size_t need = sizeof h + (size_t)h.sectors * 4 + (size_t)h.nlit * RAW;
    if (len != need || vcd_adler32(b + sizeof h, need - sizeof h) != h.body_adler) goto out;
    o = calloc(1, sizeof *o);
    if (!o) goto out;
    o->sectors = h.sectors;
    o->nlit = o->caplit = h.nlit;
    o->map = malloc((size_t)h.sectors * 4);
    o->lit = malloc((size_t)h.nlit * RAW + 1);
    if (!o->map || !o->lit) { ac3_en_overlay_free(o); o = NULL; goto out; }
    memcpy(o->map, b + sizeof h, (size_t)h.sectors * 4);
    memcpy(o->lit, b + sizeof h + (size_t)h.sectors * 4, (size_t)h.nlit * RAW);
    for (u32 k = 0; k < o->sectors; k++) {
        u32 m = o->map[k];
        if ((m & LIT_FLAG) ? (m & ~LIT_FLAG) >= o->nlit : (u64)m * RAW >= h.image_size) {
            ac3_en_overlay_free(o);
            o = NULL;
            break;
        }
    }
out:
    free(b);
    return o;
}

static void cache_save(const char *path, const cache_head_t *key, const ac3_en_overlay_t *o) {
    cache_head_t h = *key;
    h.sectors = o->sectors;
    h.nlit = o->nlit;
    h.body_adler = vcd_adler32_more(vcd_adler32((const u8 *)o->map, (size_t)o->sectors * 4),
                                    o->lit, (size_t)o->nlit * RAW);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 &&
             fwrite(o->map, 4, o->sectors, f) == o->sectors &&
             fwrite(o->lit, RAW, o->nlit, f) == o->nlit;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return; }
    remove(path);
    if (rename(tmp, path) != 0) remove(tmp);
}

ac3_en_overlay_t *ac3_en_overlay_open(const char *image_path, const char *patch_path, int *from_cache) {
    char cpath[1100];
    snprintf(cpath, sizeof cpath, "%s.en.cache", image_path);
    cache_head_t key;
    int have_key = cache_key(image_path, patch_path, &key);
    *from_cache = 0;
    if (have_key) {
        ac3_en_overlay_t *o = cache_load(cpath, &key);
        if (o) { *from_cache = 1; return o; }
    }
    ac3_en_overlay_t *o = ac3_en_overlay_build(image_path, patch_path);
    if (o && have_key) cache_save(cpath, &key, o);
    return o;
}

u32 ac3_en_overlay_sectors(const ac3_en_overlay_t *o) { return o ? o->sectors : 0; }

u32 ac3_en_overlay_literals(const ac3_en_overlay_t *o) { return o ? o->nlit : 0; }

const u8 *ac3_en_overlay_sector(const ac3_en_overlay_t *o, u32 lba, u32 *src_lba) {
    *src_lba = NO_HINT;
    if (!o || lba >= o->sectors) return NULL;
    u32 m = o->map[lba];
    if (m & LIT_FLAG) return o->lit + (size_t)(m & ~LIT_FLAG) * RAW;
    *src_lba = m;
    return NULL;
}

void ac3_en_overlay_free(ac3_en_overlay_t *o) {
    if (!o) return;
    free(o->map);
    free(o->lit);
    free(o);
}
