#include "hle.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p)          _mkdir(p)
#define GETCWD(b, n)      _getcwd((b), (int)(n))
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p)          mkdir((p), 0755)
#define GETCWD(b, n)      getcwd((b), (n))
#endif

#define CARD_FRAME_SIZE 128
#define CARD_FRAMES     1024
#define CARD_SIZE       (CARD_FRAME_SIZE * CARD_FRAMES)

#define CARD_BLOCK_SIZE  8192
#define CARD_DATA_BLOCKS 15

static void card_format_image(int slot);
static int  card_formatted(int slot);
static void card_flush(int slot);
static u8  *frame_at(int slot, int frame);

#define EV_CLASS_HW_CARD 0xF0000011u
#define EV_CLASS_SW_CARD 0xF4000001u
#define EV_SPEC_IOE      0x0004u
#define EV_SPEC_TIMEOUT  0x0100u
#define EV_SPEC_UNKNOWN  0x0200u
#define EV_SPEC_NEW      0x2000u
#define EV_SPEC_ERROR    0x8000u

extern void ac3_deliver_event(ctx_t *ctx, u32 cls, u32 spec);

static u8 card_data[2][CARD_SIZE];
static int card_loaded[2];
static int card_started;
static int card_unconfirmed[2];
static u32 card_last_chan;
static const char *card_path[2] = { "saves/card1.mcd", "saves/card2.mcd" };
static int card_flush_failed[2];

void ac3_card_set_path(int slot, const char *path) {
    if (slot < 0 || slot > 1 || !path) return;
    card_path[slot] = path;
    card_loaded[slot] = 0;
    card_flush_failed[slot] = 0;
}

const char *ac3_card_path(int slot) {
    return (slot >= 0 && slot <= 1) ? card_path[slot] : NULL;
}

static const char *card_abs(const char *path) {
    static char abs[600];
    char cwd[512];
    int rooted = path[0] == '/' || path[0] == '\\' ||
                 (path[0] && path[1] == ':');
    if (rooted || !GETCWD(cwd, sizeof cwd)) return path;
    snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    return abs;
}

static void dir_make_parents(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/' && buf[i] != '\\') continue;
        if (buf[i - 1] == ':') continue;
        char sep = buf[i];
        buf[i] = 0;
        MKDIR(buf);
        buf[i] = sep;
    }
}

static void card_ensure(int slot) {
    if (card_loaded[slot]) return;
    card_loaded[slot] = 1;
    card_unconfirmed[slot] = 1;
    memset(card_data[slot], 0, CARD_SIZE);
    FILE *f = fopen(card_path[slot], "rb");
    if (f) {
        size_t got = fread(card_data[slot], 1, CARD_SIZE, f);
        fclose(f);
        printf("memory card %d: loaded %zu bytes from %s\n",
               slot + 1, got, card_abs(card_path[slot]));
    }
    if (!card_formatted(slot)) {
        card_format_image(slot);
        printf("memory card %d: %s at %s\n", slot + 1,
               f ? "unformatted, formatting" : "new card, formatted empty",
               card_abs(card_path[slot]));
        card_flush(slot);
    }
}

/* Called after every successful write of a card image. The native build has
 * nothing to do; the web build sends the card to the page to keep it. */
__attribute__((weak)) void ac3_card_flushed(int slot, const char *path) { (void)slot; (void)path; }

static void card_flush(int slot) {
    dir_make_parents(card_path[slot]);
    FILE *f = fopen(card_path[slot], "wb");
    size_t put = 0;
    int bad = 1;
    if (f) {
        put = fwrite(card_data[slot], 1, CARD_SIZE, f);
        bad = ferror(f) != 0;
        if (fclose(f) != 0) bad = 1;
        if (put != CARD_SIZE) bad = 1;
    }
    if (!bad) { card_flush_failed[slot] = 0; ac3_card_flushed(slot, card_path[slot]); return; }

    if (card_flush_failed[slot]) return;
    card_flush_failed[slot] = 1;
    if (!f)
        hle_warn_always("card", "cannot write %s (%s) -- saves will NOT persist",
                        card_abs(card_path[slot]), strerror(errno));
    else
        hle_warn_always("card", "short write to %s (%zu of %d bytes) -- "
                        "saves will NOT persist",
                        card_abs(card_path[slot]), put, CARD_SIZE);
}

static int chan_slot(u32 chan) { return (chan & 0x10) ? 1 : 0; }

static void card_done(ctx_t *ctx, u32 chan, u32 cls, u32 spec) {
    card_last_chan = chan;
    ac3_deliver_event(ctx, cls, spec);
}

void psyq_InitCARD(ctx_t *ctx) {
    card_ensure(0);
    hle_trace("InitCARD", "pad_enable=%u", A0);
    RET(0);
}
void psyq_InitCARD2(ctx_t *ctx) { psyq_InitCARD(ctx); }

void psyq_StartCARD(ctx_t *ctx) {
    card_started = 1;
    card_ensure(0);
    RET(1);
}
void psyq_StartCARD2(ctx_t *ctx) { psyq_StartCARD(ctx); }

void psyq_StopCARD(ctx_t *ctx)  { card_started = 0; RET(1); }
void psyq_StopCARD2(ctx_t *ctx) { psyq_StopCARD(ctx); }
void psyq__ExitCard(ctx_t *ctx) { card_started = 0; RET(1); }

void psyq__card_info(ctx_t *ctx) {
    int slot = chan_slot(A0);
    card_ensure(slot);
    hle_trace("_card_info", "chan 0x%02X -> slot %d%s  ra 0x%08X", A0, slot,
              card_unconfirmed[slot] ? " (unconfirmed)" : "", ctx->r[31]);
    card_done(ctx, A0, EV_CLASS_SW_CARD,
              card_unconfirmed[slot] ? EV_SPEC_NEW : EV_SPEC_IOE);
    RET(1);
}

void psyq__card_load(ctx_t *ctx) {
    int slot = chan_slot(A0);
    card_ensure(slot);
    hle_trace("_card_load", "chan 0x%02X -> slot %d  ra 0x%08X", A0, slot,
              ctx->r[31]);
    card_done(ctx, A0, EV_CLASS_SW_CARD,
              card_formatted(slot) ? EV_SPEC_IOE : EV_SPEC_NEW);
    RET(1);
}

void psyq__card_status(ctx_t *ctx) {
    hle_trace("_card_status", "drv %u", A0);
    RET(0x01);
}

void psyq__card_wait(ctx_t *ctx) {
    hle_trace("_card_wait", "drv %u", A0);
    RET(1);
}

void psyq__card_read(ctx_t *ctx) {
    int slot = chan_slot(A0);
    u32 frame = A1, dst = A2;
    card_ensure(slot);
    if (frame >= CARD_FRAMES) {
        hle_warn("_card_read", "frame %u out of range", frame);
        RET(0);
        return;
    }
    u8 *g = (u8 *)guest_ptr(dst, CARD_FRAME_SIZE);
    if (!g) { hle_warn("_card_read", "bad destination 0x%08X", dst); RET(0); return; }
    memcpy(g, card_data[slot] + frame * CARD_FRAME_SIZE, CARD_FRAME_SIZE);
    hle_trace("_card_read", "slot %d frame %u -> 0x%08X", slot, frame, dst);
    card_done(ctx, A0, EV_CLASS_HW_CARD, EV_SPEC_IOE);
    RET(1);
}

void psyq__card_write(ctx_t *ctx) {
    int slot = chan_slot(A0);
    u32 frame = A1, src = A2;
    card_ensure(slot);
    if (frame >= CARD_FRAMES) {
        hle_warn("_card_write", "frame %u out of range", frame);
        RET(0);
        return;
    }
    const u8 *g = (const u8 *)guest_ptr(src, CARD_FRAME_SIZE);
    if (!g) { hle_warn("_card_write", "bad source 0x%08X", src); RET(0); return; }
    memcpy(card_data[slot] + frame * CARD_FRAME_SIZE, g, CARD_FRAME_SIZE);
    card_unconfirmed[slot] = 0;
    card_flush(slot);
    hle_trace("_card_write", "slot %d frame %u <- 0x%08X", slot, frame, src);
    card_done(ctx, A0, EV_CLASS_HW_CARD, EV_SPEC_IOE);
    RET(1);
}

void psyq__card_clear(ctx_t *ctx) {
    int slot = chan_slot(A0);
    card_ensure(slot);
    hle_trace("_card_clear", "chan 0x%02X -> slot %d  ra 0x%08X", A0, slot,
              ctx->r[31]);
    card_unconfirmed[slot] = 0;
    memcpy(frame_at(slot, 63), frame_at(slot, 0), CARD_FRAME_SIZE);
    card_flush(slot);
    card_done(ctx, A0, EV_CLASS_HW_CARD, EV_SPEC_IOE);
    RET(1);
}

void psyq__card_format(ctx_t *ctx) {
    int slot = chan_slot(A0);
    card_ensure(slot);
    hle_trace("_card_format", "chan 0x%02X -> slot %d", A0, slot);
    card_format_image(slot);
    card_unconfirmed[slot] = 0;
    card_flush(slot);
    RET(1);
}

void psyq__card_chan(ctx_t *ctx) { RET(card_last_chan); }

static u32 card_auto = 1;
void psyq__card_auto(ctx_t *ctx) {
    u32 prev = card_auto;
    card_auto = A0;
    RET(prev);
}

void psyq__new_card(ctx_t *ctx) { RET(1); }

#define LINK_NONE   0xFFFFu

#define DE_STATE    0x00
#define DE_SIZE     0x04
#define DE_LINK     0x08
#define DE_NAME     0x0A
#define DE_NAMEMAX  20

#define ST_FREE     0xA0u
#define ST_FIRST    0x51u
#define ST_MIDDLE   0x52u
#define ST_LAST     0x53u

#define O_READ      0x0001u
#define O_WRITE     0x0002u
#define O_CREATE    0x0200u
#define O_NOWAIT    0x8000u

static u8 *frame_at(int slot, int frame) {
    return card_data[slot] + frame * CARD_FRAME_SIZE;
}

static u8 *dirent_at(int slot, int block) { return frame_at(slot, block); }

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static void wr32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static void frame_seal(u8 *f) {
    u8 x = 0;
    for (int i = 0; i < CARD_FRAME_SIZE - 1; i++) x ^= f[i];
    f[CARD_FRAME_SIZE - 1] = x;
}

static int card_formatted(int slot) {
    return card_data[slot][0] == 'M' && card_data[slot][1] == 'C';
}

static void card_format_image(int slot) {
    if (card_formatted(slot)) {
        int files = 0;
        for (int b = 1; b <= CARD_DATA_BLOCKS; b++)
            if ((rd32(dirent_at(slot, b) + DE_STATE) & 0xFFu) == ST_FIRST) files++;
        if (files)
            hle_warn_always("card", "formatting slot %d discards %d file(s)",
                            slot + 1, files);
    }
    memset(card_data[slot], 0, CARD_SIZE);

    u8 *hdr = frame_at(slot, 0);
    hdr[0] = 'M'; hdr[1] = 'C';
    frame_seal(hdr);

    for (int b = 1; b <= CARD_DATA_BLOCKS; b++) {
        u8 *e = dirent_at(slot, b);
        wr32(e + DE_STATE, ST_FREE);
        wr16(e + DE_LINK, LINK_NONE);
        frame_seal(e);
    }
    for (int f = 16; f < 36; f++) {
        u8 *e = frame_at(slot, f);
        wr32(e + 0, 0xFFFFFFFFu);
        wr16(e + 8, LINK_NONE);
        frame_seal(e);
    }
    memcpy(frame_at(slot, 63), hdr, CARD_FRAME_SIZE);
}

static int name_eq(const u8 *stored, const char *want) {
    for (int i = 0; i < DE_NAMEMAX; i++) {
        char a = (char)stored[i], b = want[i];
        if (a != b) return 0;
        if (!a) return 1;
        if (!b) return 0;
    }
    return want[DE_NAMEMAX] == 0;
}

static int dir_find(int slot, const char *name) {
    for (int b = 1; b <= CARD_DATA_BLOCKS; b++) {
        const u8 *e = dirent_at(slot, b);
        if ((rd32(e + DE_STATE) & 0xFFu) != ST_FIRST) continue;
        if (name_eq(e + DE_NAME, name)) return b;
    }
    return 0;
}

static int dir_create(int slot, const char *name, int blocks) {
    int chain[CARD_DATA_BLOCKS], n = 0;
    for (int b = 1; b <= CARD_DATA_BLOCKS && n < blocks; b++)
        if ((rd32(dirent_at(slot, b) + DE_STATE) & 0xFFu) == ST_FREE)
            chain[n++] = b;
    if (n < blocks) return 0;

    for (int i = 0; i < blocks; i++) {
        u8 *e = dirent_at(slot, chain[i]);
        memset(e, 0, CARD_FRAME_SIZE);
        wr32(e + DE_STATE, i == 0 ? ST_FIRST
                                  : (i == blocks - 1 ? ST_LAST : ST_MIDDLE));
        wr32(e + DE_SIZE, i == 0 ? (u32)blocks * CARD_BLOCK_SIZE : 0u);
        wr16(e + DE_LINK, (i + 1 < blocks) ? (u32)(chain[i + 1] - 1) : LINK_NONE);
        if (i == 0) {
            size_t len = strlen(name);
            if (len > DE_NAMEMAX) len = DE_NAMEMAX;
            memcpy(e + DE_NAME, name, len);
        }
        frame_seal(e);
        memset(card_data[slot] + chain[i] * CARD_BLOCK_SIZE, 0, CARD_BLOCK_SIZE);
    }
    return chain[0];
}

static int block_step(int slot, int first, u32 index) {
    int b = first;
    while (index-- && b) {
        u32 link = rd16(dirent_at(slot, b) + DE_LINK);
        b = (link == LINK_NONE || link >= CARD_DATA_BLOCKS) ? 0 : (int)link + 1;
    }
    return b;
}

static int path_split(u32 addr, int *slot, char *name, size_t cap) {
    char raw[64];
    size_t n = 0;
    while (n < sizeof raw - 1) {
        u8 c = (u8)MEM_R8U(addr + (u32)n);
        if (!c) break;
        raw[n++] = (char)c;
    }
    raw[n] = 0;

    const char *colon = strchr(raw, ':');
    *slot = 0;
    if (colon) {
        if (colon - raw >= 3 && raw[2] == '1') *slot = 1;
        colon++;
    } else {
        colon = raw;
    }
    size_t len = strlen(colon);
    if (len >= cap) len = cap - 1;
    memcpy(name, colon, len);
    name[len] = 0;
    return raw[0] != 0;
}

static int name_match(const char *pat, const u8 *stored) {
    char nm[DE_NAMEMAX + 1];
    memcpy(nm, stored, DE_NAMEMAX);
    nm[DE_NAMEMAX] = 0;

    const char *n = nm;
    if (!*pat) return 1;
    for (; *pat; pat++, n++) {
        if (*pat == '*') return 1;
        if (*pat == '?') { if (!*n) return 0; continue; }
        if (*pat != *n) return 0;
    }
    return *n == 0;
}

typedef struct {
    int used;
    int slot;
    int first;
    u32 pos;
    u32 size;
    int async;
} card_fd_t;

static void file_done(ctx_t *ctx, card_fd_t *f, int ok) {
    if (!f->async) return;
    card_done(ctx, f->slot ? 0x10u : 0x00u, EV_CLASS_SW_CARD,
              ok ? EV_SPEC_IOE : EV_SPEC_ERROR);
}

static card_fd_t fds[8];

static card_fd_t *fd_get(u32 fd) {
    if (fd >= sizeof fds / sizeof fds[0] || !fds[fd].used) return NULL;
    return &fds[fd];
}

void psyq_open(ctx_t *ctx) {
    char name[64];
    int slot;
    u32 flags = A1;

    if (!path_split(A0, &slot, name, sizeof name) || !name[0]) {
        hle_warn("open", "unusable path at 0x%08X", A0);
        RET(-1);
        return;
    }
    card_ensure(slot);

    int first = dir_find(slot, name);
    if (!first && (flags & O_CREATE)) {
        int blocks = (int)((flags >> 16) & 0xFFFFu);
        if (blocks < 1) blocks = 1;
        if (blocks > CARD_DATA_BLOCKS) blocks = CARD_DATA_BLOCKS;
        first = dir_create(slot, name, blocks);
        if (!first) {
            hle_warn("open", "no room on card %d for %s (%d blocks)",
                     slot + 1, name, blocks);
            RET(-1);
            return;
        }
        card_flush(slot);
        hle_trace("open", "created %s on card %d, %d blocks", name, slot + 1,
                  blocks);
    }
    if (!first) {
        hle_trace("open", "%s not found on card %d", name, slot + 1);
        RET(-1);
        return;
    }

    for (u32 i = 0; i < sizeof fds / sizeof fds[0]; i++) {
        if (fds[i].used) continue;
        fds[i].used = 1;
        fds[i].slot = slot;
        fds[i].first = first;
        fds[i].pos = 0;
        fds[i].size = rd32(dirent_at(slot, first) + DE_SIZE);
        fds[i].async = (flags & O_NOWAIT) != 0;
        hle_trace("open", "%s -> fd %u, %u bytes%s", name, i, fds[i].size,
                  fds[i].async ? ", async" : "");
        RET(i);
        return;
    }
    hle_warn("open", "too many open files");
    RET(-1);
}

void psyq_close(ctx_t *ctx) {
    card_fd_t *f = fd_get(A0);
    hle_trace("close", "fd %u%s", A0, f ? "" : " (not open)");
    if (!f) { RET(-1); return; }
    f->used = 0;
    RET(A0);
}

void psyq_lseek(ctx_t *ctx) {
    card_fd_t *f = fd_get(A0);
    if (!f) { RET(-1); return; }
    s32 off = (s32)A1;
    u32 pos;
    switch (A2) {
    case 0:  pos = (u32)off; break;
    case 1:  pos = f->pos + (u32)off; break;
    case 2:  pos = f->size + (u32)off; break;
    default: RET(-1); return;
    }
    if (pos > f->size) { RET(-1); return; }
    f->pos = pos;
    RET(pos);
}

static u32 file_io(card_fd_t *f, u32 guest, u32 len, int writing) {
    u32 done = 0;
    if (f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = f->size - f->pos;

    while (done < len) {
        int block = block_step(f->slot, f->first, f->pos / CARD_BLOCK_SIZE);
        if (!block) break;
        u32 in_block = f->pos % CARD_BLOCK_SIZE;
        u32 chunk = CARD_BLOCK_SIZE - in_block;
        if (chunk > len - done) chunk = len - done;

        u8 *card = card_data[f->slot] + block * CARD_BLOCK_SIZE + in_block;
        void *host = guest_ptr(guest + done, chunk);
        if (!host) {
            hle_warn(writing ? "write" : "read", "bad buffer 0x%08X",
                     guest + done);
            break;
        }
        if (writing) memcpy(card, host, chunk);
        else         memcpy(host, card, chunk);

        f->pos += chunk;
        done += chunk;
    }
    return done;
}

void psyq_read(ctx_t *ctx) {
    card_fd_t *f = fd_get(A0);
    if (!f) { RET(-1); return; }
    u32 n = file_io(f, A1, A2, 0);
    hle_trace("read", "fd %u -> 0x%08X, %u of %u bytes", A0, A1, n, A2);
    file_done(ctx, f, n == A2);
    RET(n);
}

void psyq_write(ctx_t *ctx) {
    card_fd_t *f = fd_get(A0);
    if (!f) { RET(-1); return; }
    u32 n = file_io(f, A1, A2, 1);
    card_flush(f->slot);
    hle_trace("write", "fd %u <- 0x%08X, %u of %u bytes", A0, A1, n, A2);
    file_done(ctx, f, n == A2);
    RET(n);
}

#define DIR_NAME   0x00
#define DIR_ATTR   0x14
#define DIR_SIZE   0x18
#define DIR_NEXT   0x1C
#define DIR_HEAD   0x20
#define DIR_SYSTEM 0x24

static int  scan_slot;
static int  scan_block;
static char scan_pattern[64];

static void fill_direntry(u32 out, int slot, int block) {
    const u8 *e = dirent_at(slot, block);
    u8 *d = (u8 *)guest_ptr(out, 0x28);
    if (!d) { hle_warn("firstfile", "bad DIRENTRY 0x%08X", out); return; }
    memset(d, 0, 0x28);
    memcpy(d + DIR_NAME, e + DE_NAME, DE_NAMEMAX);
    wr32(d + DIR_SIZE, rd32(e + DE_SIZE));
    wr32(d + DIR_HEAD, (u32)block);
}

static int scan_next(int slot, const char *pat, int from) {
    for (int b = from; b <= CARD_DATA_BLOCKS; b++) {
        const u8 *e = dirent_at(slot, b);
        if ((rd32(e + DE_STATE) & 0xFFu) != ST_FIRST) continue;
        if (name_match(pat, e + DE_NAME)) return b;
    }
    return 0;
}

void psyq_firstfile(ctx_t *ctx) {
    int slot;
    if (!path_split(A0, &slot, scan_pattern, sizeof scan_pattern)) {
        RET(0);
        return;
    }
    card_ensure(slot);
    scan_slot = slot;

    int b = scan_next(slot, scan_pattern, 1);
    hle_trace("firstfile", "card %d pattern \"%s\" -> block %d",
              slot + 1, scan_pattern, b);
    if (!b) { scan_block = 0; RET(0); return; }
    scan_block = b + 1;
    fill_direntry(A1, slot, b);
    RET(A1);
}

void psyq_firstfile2(ctx_t *ctx) { psyq_firstfile(ctx); }

void psyq_nextfile(ctx_t *ctx) {
    if (!scan_block) { RET(0); return; }
    int b = scan_next(scan_slot, scan_pattern, scan_block);
    if (!b) { scan_block = 0; RET(0); return; }
    scan_block = b + 1;
    fill_direntry(A0, scan_slot, b);
    RET(A0);
}
