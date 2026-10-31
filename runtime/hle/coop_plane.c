/* coop_plane.c -- the guest's own plane model (docs/coop/S4_plane_choice.md, step C).
 *
 * A mission loads one plane: the host's. Its file (66 + plane) is read by
 * FN_PLANE_LOAD and taken apart by 0x80035F98: four entries, relocated
 * (FN_RELOC); entry 3 (sounds) to the sound chip, entry 2 (textures) to video
 * memory (FN_LOAD_TEXTURES: one texture page at (704, 256), its colour tables
 * at (336, 312..319), a HUD icon), entries 0 and 1 decompressed (FN_DECOMPRESS) into the heap. Entry
 * 0 is the model's parts table (MODEL_PARTS, also the draw table's player
 * entry), entry 1 a second object (MODEL_OBJECT).
 *
 * When the guest picked another plane, its file is read too, into the extra
 * memory past the PS1's 2 MB (ac3_ext, 0x80200000): queued right after the
 * host's, so it arrives during the mission's loading. When the guest is
 * created it is taken apart the same way, there, with the game's own
 * functions; its textures go to the second video memory bank (gpu_sw.c,
 * same places), and its model's faces are marked to read them from there
 * (texture page bit 11, colour table bit 15). Its sounds are not loaded: the
 * guest's plane sounds like the host's.
 *
 * Drawing: FN_PLAYER_DRAW draws both players' planes. For each it copies
 * pointers out of the player's parts table into a record: "the player"
 * (slot 0) into one, "the other player" into another. Afterwards the
 * guest's record gets the matching pointers from the guest's table (the
 * host's frame: the other player's record; the guest's view replay: the
 * player's). */
#include "coop_internal.h"

#define FN_PLANE_LOAD     0x80035F18u   /* queues the player's plane file */
#define FN_FILE_SIZE      0x8002C5E0u   /* (file) -> bytes */
#define FN_FILE_QUEUE     0x8002C7ACu   /* (file, buffer, callback): queue a disc read */
#define FILE_QUEUE_PTR    (GAME_GP + 0x48u)   /* -> the read queue; +0x17C, +0x180: the last request's buffer and end */
#define FN_RELOC          0x80027338u   /* (archive): halfword count, then offsets -> pointers; returns the entries */
#define FN_RELOC_N        0x80027380u   /* (base, entries, n) */
#define FN_DECOMPRESS     0x800AFD08u   /* (src, dst) -> end of dst */
#define FN_LOAD_TEXTURES  0x800272F0u   /* (entries, end): decompress, then LoadImage each picture and colour table */
#define FN_LOAD_IMAGE     0x800A8440u   /* Psy-Q LoadImage(&rect, data) */
#define PLANE_PAGE        0x1Bu         /* a face's texture page: (704, 256) */
#define PLANE_CLUT_X      21u           /* its colour tables: x 336 / 16, */
#define PLANE_CLUT_Y0     312u          /*   rows 312..319 */
#define FACE_SIZE         0x24u
#define FACE_CLUT         0x0Eu         /* in a face: uv0 + colour table (+0x0C), uv1 + texture page (+0x10) */
#define FACE_PAGE         0x12u
#define FN_PLAYER_DRAW    0x80067494u   /* (draw state, list, ..., player record, [other record]) */
#define PLANE_FILES       0x800B0B84u   /* halfword per plane: its file */
#define MODEL_PTR         (GAME_GP + 0x2DCu)  /* -> the player's model state: */
#define MODEL_PARTS       0x08u         /*   -> parts table (word count, then one pointer per part) */
#define MODEL_OBJECT      0x0Cu         /*   -> entry 1 */
#define PART_LODS         3u            /* each part: +4, +8, +0xC: its three levels of detail */

#define EXT_FILE          0x80200000u   /* the guest's plane file, as read */
#define EXT_FILE_MAX      0x40000u
#define EXT_MODEL         0x80240000u   /* ... and its decompressed model */
#define EXT_END           (0x80200000u + AC3_EXT_SIZE)
#define MAP_MAX           64u

#include "gpu_sw.h"

static struct {
    int queued, ready, to_bank;
    int plane;
    u32 parts, object;            /* the guest's parts table and entry 1 */
    u32 n;                        /* host pointer -> guest pointer */
    u32 from[MAP_MAX], to[MAP_MAX];
} M;

/* the host's plane file has been queued: queue the guest's after it */
static int plane_load(ctx_t *ctx) {
    ac3_interpret(ctx, FN_PLANE_LOAD);
    M.queued = M.ready = 0;
    M.n = 0;
    int plane = ac3_coop_guest_plane;
    if (plane < 0 || plane > 36 || plane == MEM_R16S(SCENE + SCENE_PLANE)) return 1;   /* the host's model will do */
    ac3_ext_enable();
    ctx_t after = *ctx;
    u32 file = MEM_R16U(PLANE_FILES + 2u * (u32)plane);
    call_guest(ctx, FN_FILE_SIZE, file, 0, 0, 0);
    u32 size = ctx->r[2];
    if (size == 0 || size > EXT_FILE_MAX) { printf("coop: guest's plane file %u: %u bytes, not loaded\n", file, size); *ctx = after; return 1; }
    u32 q = MEM_R32(FILE_QUEUE_PTR), last = 0, last_end = 0;
    if (ram_ptr(q)) { last = MEM_R32(q + 0x17C); last_end = MEM_R32(q + 0x180); }
    for (u32 i = 0; i < size; i += 4) MEM_W32(EXT_FILE + i, 0);
    call_guest(ctx, FN_FILE_QUEUE, file, EXT_FILE, 0, 0);
    if (ram_ptr(q)) { MEM_W32(q + 0x17C, last); MEM_W32(q + 0x180, last_end); }   /* the loader waits for the host's */
    *ctx = after;
    M.queued = 1;
    M.plane = plane;
    printf("coop: guest's plane %d: file %u (%u bytes) queued\n", plane, file, size);
    return 1;
}

static void map_add(u32 from, u32 to) {
    if (M.n < MAP_MAX && from && to) { M.from[M.n] = from; M.to[M.n] = to; M.n++; }
}

/* The guest was created: take its plane file apart as 0x80035F98 does
 * (without the sounds; textures: step C4). */
void coop_plane_guest_created(ctx_t *ctx) {
    M.ready = 0;
    if (!M.queued) return;
    M.queued = 0;
    if (MEM_R16U(EXT_FILE) != 4u) { printf("coop: guest's plane file not read (yet)\n"); return; }
    u32 model = MEM_R32(MODEL_PTR);
    if (!ram_ptr(model)) return;
    ctx_t saved = *ctx;
    call_guest(ctx, FN_RELOC, EXT_FILE, 0, 0, 0);
    u32 entries = ctx->r[2];
    u32 e0 = MEM_R32(entries), e1 = MEM_R32(entries + 4), e2 = MEM_R32(entries + 8), e3 = MEM_R32(entries + 12);
    M.to_bank = 1;                                     /* its pictures to the second bank */
    call_guest(ctx, FN_LOAD_TEXTURES, e2, e3, 0, 0);
    M.to_bank = 0;
    call_guest(ctx, FN_DECOMPRESS, e0, EXT_MODEL, 0, 0);
    u32 object = ctx->r[2];
    call_guest(ctx, FN_DECOMPRESS, e1, object, 0, 0);
    u32 end = ctx->r[2];
    if (end <= object || end > EXT_END) { printf("coop: guest's plane model did not fit\n"); *ctx = saved; return; }
    call_guest(ctx, FN_RELOC, EXT_MODEL, 0, 0, 0);
    u32 parts = ctx->r[2], count = MEM_R32(EXT_MODEL);
    for (u32 i = 0; i < count && i < 32; i++) {
        u32 p = MEM_R32(parts + 4u * i);
        call_guest(ctx, FN_RELOC_N, p, p + 4u, 6, 0);
    }
    *ctx = saved;
    /* the host's pointers -> the guest's, part by part */
    u32 hp = MEM_R32(model + MODEL_PARTS), hcount = MEM_R32(SCENE + 0x1040u);
    for (u32 i = 0; i < count && i < hcount; i++) {
        u32 h = MEM_R32(hp + 4u * i), g = MEM_R32(parts + 4u * i);
        map_add(h, g);
        for (u32 k = 1; k <= PART_LODS; k++) map_add(MEM_R32(h + 4u * k), MEM_R32(g + 4u * k));
    }
    /* its faces: the plane's texture page and colour tables -> the second bank */
    u32 faces = 0;
    for (u32 a = EXT_MODEL; a + FACE_PAGE + 2 <= end; a += 2) {
        u32 clut = MEM_R16U(a), page = MEM_R16U(a + (FACE_PAGE - FACE_CLUT));
        u32 cy = (clut >> 6) & 0x1FFu;
        if ((clut & 0x3Fu) == PLANE_CLUT_X && cy >= PLANE_CLUT_Y0 && cy < PLANE_CLUT_Y0 + 8u && !(clut & 0x8000u) &&
            (page & 0x1Fu) == PLANE_PAGE && page < 0x200u) {
            MEM_W16(a, clut | 0x8000u);
            MEM_W16(a + (FACE_PAGE - FACE_CLUT), page | 0x800u);
            faces++;
            a += FACE_SIZE - 2;
        }
    }
    M.parts = parts;
    M.object = object;
    M.ready = 1;
    printf("coop: guest's plane %d ready: %u parts (host %u), model %08X..%08X, %u pointers mapped, %u faces on the second bank\n",
           M.plane, count, hcount, EXT_MODEL, end, M.n, faces);
}

/* one player's record from FN_PLAYER_DRAW: the host's pointers -> the guest's */
static void fix_record(u32 rec, u32 len) {
    for (u32 o = 0; o + 4 <= len; o += 4) {
        u32 v = MEM_R32(rec + o);
        for (u32 i = 0; i < M.n; i++)
            if (v == M.from[i]) { MEM_W32(rec + o, M.to[i]); break; }
    }
}

#define RECORD_LEN 0x100u
static int player_draw(ctx_t *ctx) {
    u32 sp = ctx->r[29];
    u32 self = ctx->r[7], other = MEM_R32(sp + 0x10u);   /* a3: the player's record; stack: the other player's */
    ac3_interpret(ctx, FN_PLAYER_DRAW);
    fix_record(G.replaying ? self : other, RECORD_LEN);
    return 1;
}

/* The player update and effects use the model's second object (weapon and
 * effect positions): the guest's goes in for its update and its view
 * replay. Returns what was there (0: nothing changed). */
u32 coop_plane_object_in(void) {
    u32 model = MEM_R32(MODEL_PTR);
    if (!M.ready || !ram_ptr(model)) return 0;
    u32 old = MEM_R32(model + MODEL_OBJECT);
    MEM_W32(model + MODEL_OBJECT, M.object);
    return old;
}
void coop_plane_object_out(u32 old) {
    u32 model = MEM_R32(MODEL_PTR);
    if (old && ram_ptr(model)) MEM_W32(model + MODEL_OBJECT, old);
}

/* while the guest's pictures are loaded: into the second bank, not the game's video memory */
static int load_image(ctx_t *ctx) {
    u32 r = ctx->r[4];
    s32 x = MEM_R16S(r), y = MEM_R16S(r + 2), w = MEM_R16S(r + 4), h = MEM_R16S(r + 6);
    u32 bytes = (u32)(w * h * 2);
    static u8 buf[0x20000];
    if (w > 0 && h > 0 && bytes <= sizeof buf) {
        for (u32 i = 0; i < bytes; i++) buf[i] = (u8)MEM_R8U(ctx->r[5] + i);
        gpu_sw_bank_load(x, y, w, h, buf);
    }
    ctx->r[2] = 0;
    return 1;
}

int coop_plane_hook(ctx_t *ctx, u32 target) {
    if (M.to_bank && target == FN_LOAD_IMAGE) return load_image(ctx);
    switch (target) {
    case FN_PLANE_LOAD:  return plane_load(ctx);
    case FN_PLAYER_DRAW: return M.ready && G.live && player_draw(ctx);
    }
    return 0;
}
