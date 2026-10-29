/* coop.c -- see coop.h. Addresses are for the Rev 1 ACE3.BIN; the reasoning
 * behind each one is in docs/coop/memory_map.md. */
#include "coop.h"
#include "gpu_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int ac3_coop_guest;
int ac3_coop_view;
int ac3_coop_split;
static int coop_debug = -1;

extern void ac3_interpret(ctx_t *ctx, u32 entry);
extern u32 ac3_vblank_count(void);

#define GAME_GP        0x800BCB74u
#define SLOT_TABLE     (GAME_GP + 0x2E8u)  /* -> 64 entity pointers */
#define PLAYER_UPDATE  0x800729DCu         /* first call of the flight tick */
#define INPUT_MAP_FN   0x80028C24u         /* (controller, map, map+0x3C): applies the button config */
#define CTRL_PORT2     0x800BF388u         /* the game's decoded record for controller port 2 */
#define TARGETING_FN   0x8003A23Cu         /* flight tick: lock-on / target selection */
#define CAM_STATE_PTR  0x800BD098u         /* -> the player's camera/target state (block+0x98) */
#define SLOT_SIZE      0x58u               /* slots 0 and 1 */
#define LIVE           0x10000u            /* handle bit: slot in use */
#define WORLD_PTR      (GAME_GP + 0x2E0u)  /* world/view state */
#define AFTER_TICK     0x80033A8Cu         /* flight frame: just after it called the tick (by pointer) */
#define DRAW_LAST_FN   0x80033814u         /* flight frame: last drawing call ... */
#define DRAW_LAST_RA   0x80033DA8u

/* Per-player memory: the guest has its own copy, exchanged with the host's
 * around the guest's update. */
static const struct { u32 addr, len; } per_player[] = {
    { 0,           0xE0u },   /* player aircraft state: where gp+0x514 points (see pp_addr) */
    { 0x800BF3F0u, 0x40u },   /* input map (incl. the 4 bytes at map+0x3C) */
    { 0x800C7BE8u, 0x10u },   /* written by 0x80072114 every frame (position copy) */
    { 0x800C6D60u, 0x18u },   /* targeting state (0x8003A23C); index PP_TARGETING */
    { 0x800C6EC0u, 0x10u },   /* targeting state (0x8003A23C) */
};
#define N_PER_PLAYER (sizeof per_player / sizeof per_player[0])
#define PP_TARGETING 3
#define PP_BLOCK     0
/* The aircraft state block moves from mission to mission (0x801771E0 in the
 * first random mission, 0x80171484 in another): always take it from the
 * game's own pointer, gp+0x514 (gp+0x518..0x524 point inside it). */
#define BLOCK_PTR    (GAME_GP + 0x514u)
static u32 pp_addr(unsigned i) { return i == PP_BLOCK ? MEM_R32(BLOCK_PTR) : per_player[i].addr; }

/* Host-only memory the player update also writes: the guest's writes are
 * discarded. world = 1: an offset from the world pointer. */
static const struct { u32 addr, len; int world; } host_only[] = {
    { 0x800C5830u, 0x40u, 0 },   /* engine sound channels */
    { 0x800BD0C0u, 0x04u, 0 },   /* a library word */
    /* written when "the player" is shot down (0x80073880..0x800738C4): */
    { 0x800BFC28u, 0x02u, 0 },   /* scene+0x958: times the player was shot down */
    { 0x800BCEA0u, 0x04u, 0 },   /* 0x800301A8: screen effect after being shot down */
    { 0x28Cu,      0x08u, 1 },   /* world+0x28C (0x8006CBA4) */
    { 0x000u,      0x04u, 1 },   /* world+0: bit 0 hides the HUD (0x80035740, from the shot-down camera 0x80079564) */
};
static u32 ho_addr(unsigned i) { return host_only[i].world ? MEM_R32(WORLD_PTR) + host_only[i].addr : host_only[i].addr; }
#define N_HOST_ONLY (sizeof host_only / sizeof host_only[0])

/* World-structure ranges only the camera and view setup write (relative to
 * the world pointer): the guest's view keeps its own copy between frames. */
static const struct { u32 off, len; } view_private[] = {
    { 0x2Cu, 0x34u },    /* 0x800332E4: view setup */
    { 0xB8u, 0xE8u },    /* the camera object (world+0x28 points here; 0x8007B438, and
                            0x800793C4 when switching camera mode writes up to +0x19C) */
};
#define N_VIEW_PRIVATE (sizeof view_private / sizeof view_private[0])
static void guest_camera_swap(u32 host, u32 mine);

/* The HUD's targets, seen from one player. Each target has a 0x34-byte
 * record (pointers at 0x800C67A0 + 4 * slot) filled relative to "the player":
 * the tick (0x80045278) stores the target's distance at +0x10 and only
 * recomputes it every 16 frames for targets over 0x84D0 away, and sets the
 * in-range/visible flags at +0x0C from it; the targeting step writes +0x2C;
 * the HUD shows and locks only targets flagged in range. Locking also takes
 * several frames: the HUD code (0x8003F2CC, 0x800409D4, 0x80040A18) keeps a
 * state and a timer in the first two bytes of the record and
 * the lock in progress at gp+0x354..0x35B (0x800BCEC8: state, target slot,
 * and the lock box's shrinking animation, 0x800409D4). The guest needs its
 * own copy of all of it, kept from one frame to the next: with the host's,
 * targets near the guest but far from the host stayed "out of range" (the
 * box flickered and jumped) and a lock never completed. */
#define HUD_GLOB       0x800BCEC8u
#define HUD_GLOB_LEN   8u
#define HUD_RECS       0x800C67A0u
#define HUD_REC_LEN    0x34u     /* the whole target record */
/* HUD state the drawing code keeps from one frame to the next, seen from one
 * player (0x8003D1A4, 0x8003DA70, 0x8003DD08, 0x8003D450, 0x800309A8:
 * per-target arrays; 0x800662B4: the gun reticle). */
static const struct { u32 addr, len; } hud_static[] = {
    { 0x800C68E0u, 0x480u },   /* 0x800C68E0 (12 bytes x 32) and 0x800C6A60 (12 bytes x 64) */
    { 0x800C6D78u, 0x08u },    /* 0x800C6D7C (0x8003D450) */
    { 0x800C6EA0u, 0x14u },    /* 0x800309A8.. */
    /* the gun reticle (0x800662B4): a ring of 32 positions of the player's
     * bullets, one per frame, and its index; the reticle is drawn where the
     * bullets fired some frames ago have got to */
    { 0x800C79C8u, 0x200u },
    { 0x800BD070u, 0x04u },    /* gp+0x4FC: ring index, gp+0x4FD: reticle entry */
};
#define N_HUD_STATIC (sizeof hud_static / sizeof hud_static[0])
static void guest_hud_swap(void);

static struct {
    int dead;              /* destroyed this mission: spectating until the next one */
    u32 deaths, last_mode, flight_table, last_update_field;
    int live;              /* guest aircraft exists in this flight */
    u32 handle;            /* its handle (slot 1) */
    u32 created_field, updates;
    u8  mine[N_PER_PLAYER][0x100];   /* the guest's copy of each per-player range */
    u8  keep[N_HOST_ONLY][0x40];
    /* the guest's view (S3) */
    int view_on, cam_valid;
    u8  cam[N_VIEW_PRIVATE][0x100];
    /* the HUD's lock-on progress (see guest_hud_swap) */
    int hud_valid;
    u8  hud_glob[HUD_GLOB_LEN];
    u8  hud_rec[64][HUD_REC_LEN];
    u8  hud_static[N_HUD_STATIC][0x480];
    u8 *snap_ram, *keep_buf;
    u8  snap_scratch[1024];
    ctx_t snap_ctx;
    u32 snap_pc;
    int replaying;
    u32 views;
} G;

static void copy_out(u32 addr, u8 *dst, u32 n) { for (u32 i = 0; i < n; i++) dst[i] = (u8)MEM_R8U(addr + i); }
static void copy_in(u32 addr, const u8 *src, u32 n) { for (u32 i = 0; i < n; i++) MEM_W8(addr + i, src[i]); }
static void exchange(u32 addr, u8 *other, u32 n) {
    for (u32 i = 0; i < n; i++) { u8 t = (u8)MEM_R8U(addr + i); MEM_W8(addr + i, other[i]); other[i] = t; }
}
static int ram_ptr(u32 p) { return (p >> 24) == 0x80u && (p & 0x1FFFFFu) < AC3_RAM_SIZE - 0x100u; }

/* Create the guest as a copy of the host's aircraft in slot 1, beside it. */
static int create_guest(u32 slots) {
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(host) || !ram_ptr(mine)) return 0;
    u32 h = MEM_R32(host);
    if (!(h & LIVE) || (MEM_R32(mine) & LIVE)) return 0;      /* no host, or slot 1 taken */
    /* Wait until the host has control (player mode 1). Before that the mission
     * moves the host's mode along (2 -> 4 -> 1) from outside the player
     * update, and a guest copied too early would stay in the intro mode. */
    u32 mode = MEM_R32(GAME_GP + 0x520u);
    if (!ram_ptr(mode) || MEM_R32(mode + 4) != 1u) return 0;
    u8 e[SLOT_SIZE];
    copy_out(host, e, SLOT_SIZE);
    copy_in(mine, e, SLOT_SIZE);
    G.handle = (h & ~0x3Fu) | 1u;
    MEM_W32(mine, G.handle);
    MEM_W32(mine + 8, MEM_R32(mine + 8) + (150u << 12));     /* 150 units to the side */
    for (unsigned i = 0; i < N_PER_PLAYER; i++) copy_out(pp_addr(i), G.mine[i], per_player[i].len);
    G.live = 1;
    G.cam_valid = 0;          /* the world (and its camera) moves between missions */
    G.hud_valid = 0;
    G.flight_table = slots;
    G.created_field = ac3_vblank_count();
    G.updates = 0;
    printf("coop: guest aircraft created in slot 1 (handle 0x%05X) at field %u\n", G.handle, G.created_field);
    return 1;
}

static void call_guest(ctx_t *ctx, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    ctx->r[4] = a0; ctx->r[5] = a1; ctx->r[6] = a2; ctx->r[7] = a3;
    ac3_interpret(ctx, fn);
}

/* The flight tick's targeting step, for the guest (the tick runs it for the
 * host after the world update; 0x80032F3C):
 *     cam = *CAM_STATE_PTR;  args = { cam[0x1C], cam[8], cam[0xC], cam[0x10], out, target }
 *     0x8003A23C(&args);     cam[0x18] = args.target   (the lock-on the launch code uses)
 * The camera part of the result (view+0xBC) is the view's business: skipped. */
static void guest_targeting(ctx_t *ctx) {
    u32 cam = MEM_R32(CAM_STATE_PTR);
    if (!ram_ptr(cam)) return;
    u32 args = (ctx->r[29] - 0x80u) & ~7u;     /* scratch below the caller's stack */
    MEM_W32(args + 0x00, MEM_R32(cam + 0x1Cu));
    MEM_W32(args + 0x04, MEM_R32(cam + 0x08u));
    MEM_W32(args + 0x08, MEM_R32(cam + 0x0Cu));
    MEM_W32(args + 0x0C, MEM_R32(cam + 0x10u));
    MEM_W32(args + 0x10, MEM_R32(cam + 0x18u));   /* not read before being written; keeps it tidy */
    MEM_W32(args + 0x14, MEM_R32(cam + 0x18u));
    u32 sp = ctx->r[29];
    ctx->r[29] = args - 0x10u;                  /* the tick's frame: args at sp+0x10 */
    call_guest(ctx, TARGETING_FN, args, 0, 0, 0);
    ctx->r[29] = sp;
    if (coop_debug > 0 && G.updates % 30u == 0)
        printf("coop: field %u guest targeting in %08X %08X %08X %08X -> out %08X target %08X\n", ac3_vblank_count(),
               MEM_R32(args), MEM_R32(args + 4), MEM_R32(args + 8), MEM_R32(args + 12), MEM_R32(args + 0x10), MEM_R32(args + 0x14));
    MEM_W32(cam + 0x18u, MEM_R32(args + 0x14));
}

/* Exchange the camera ranges of the world structure with the guest's copy.
 * The first time in a mission the guest's copy starts as the host's, pointed
 * at the guest's entity. */
static void guest_camera_swap(u32 host, u32 mine) {
    u32 w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(w)) return;
    if (!G.cam_valid) {
        for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) {
            copy_out(w + view_private[i].off, G.cam[i], view_private[i].len);
            for (u32 o = 0; o + 4 <= view_private[i].len; o += 4) {
                u8 *b = G.cam[i] + o;
                u32 v = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
                if (v >= host && v < host + SLOT_SIZE) {
                    v = mine + (v - host);
                    b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24);
                }
            }
        }
        G.cam_valid = 1;
    }
    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) exchange(w + view_private[i].off, G.cam[i], view_private[i].len);
}

/* Exchange the HUD's lock-on state with the guest's copy (which starts as a
 * copy of the host's, the first time in a mission). */
static void guest_hud_swap(void) {
    if (!G.hud_valid) {
        copy_out(HUD_GLOB, G.hud_glob, HUD_GLOB_LEN);
        for (unsigned i = 0; i < N_HUD_STATIC; i++) copy_out(hud_static[i].addr, G.hud_static[i], hud_static[i].len);
        for (u32 t = 0; t < 64u; t++) {
            u32 r = MEM_R32(HUD_RECS + 4u * t);
            if (ram_ptr(r)) copy_out(r, G.hud_rec[t], HUD_REC_LEN);
        }
        G.hud_valid = 1;
    }
    exchange(HUD_GLOB, G.hud_glob, HUD_GLOB_LEN);
    for (unsigned i = 0; i < N_HUD_STATIC; i++) exchange(hud_static[i].addr, G.hud_static[i], hud_static[i].len);
    for (u32 t = 0; t < 64u; t++) {
        u32 r = MEM_R32(HUD_RECS + 4u * t);
        if (ram_ptr(r)) exchange(r, G.hud_rec[t], HUD_REC_LEN);
    }
}

static void guest_update(ctx_t *ctx, const ctx_t *entry) {
    if (coop_debug < 0) { const char *e = getenv("AC3_COOP_DEBUG"); coop_debug = e && *e && *e != '0'; }
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return;
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(host) || !ram_ptr(mine) || !ram_ptr(pp_addr(PP_BLOCK))) return;
    G.last_update_field = ac3_vblank_count();

    /* Destroyed (crash, shot down): the game cleared its live bit. The guest
     * watches the host until the next mission. */
    if (G.live && !(MEM_R32(mine) & LIVE)) {
        printf("coop: guest aircraft destroyed at field %u\n", ac3_vblank_count());
        G.live = 0;
        G.dead = 1;
        G.deaths++;
    }
    if (G.live && (MEM_R32(mine) & ~LIVE) != (G.handle & ~LIVE)) {   /* slot 1 taken over (not seen so far) */
        printf("coop: slot 1 reused by the game at field %u (0x%05X)\n", ac3_vblank_count(), MEM_R32(mine));
        G.live = 0;
    }
    if (!G.live && (G.dead || !create_guest(slots))) return;

    /* swap in the guest */
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_out(ho_addr(i), G.keep[i], host_only[i].len);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(pp_addr(i), G.mine[i], per_player[i].len);
    MEM_W32(slots, mine);
    MEM_W32(slots + 4, host);

    /* The player update also writes the camera: when "the player" is shot
     * down it switches it to follow the explosion (0x80077878 writes
     * world+0xB8). So the guest's update works on the guest's camera too. */
    guest_camera_swap(host, mine);
    guest_hud_swap();

    ctx_t saved = *ctx;
    call_guest(ctx, INPUT_MAP_FN, CTRL_PORT2, 0x800BF3F0u, 0x800BF42Cu, 0);
    if (coop_debug && G.updates % 30u == 0) {
        printf("coop: field %u guest map", ac3_vblank_count());
        for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(0x800BF3F0u + i));
        printf("\n");
    }
    if (!getenv("AC3_COOP_NOUPDATE")) {     /* research switch: create the guest but never update it */
        call_guest(ctx, PLAYER_UPDATE, entry->r[4], entry->r[5], entry->r[6], entry->r[7]);
        guest_targeting(ctx);
    }
    if (coop_debug && G.updates % 30u == 0) {
        u32 e = MEM_R32(slots);   /* still the guest's entity here */
        printf("coop: field %u guest missiles %u (host %u)\n", ac3_vblank_count(),
               MEM_R16U(pp_addr(PP_BLOCK) + 0xB4u), (u32)(G.mine[0][0xB4] | G.mine[0][0xB5] << 8));
        printf("coop: field %u guest block", ac3_vblank_count());
        for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(pp_addr(PP_BLOCK) + i));
        printf("\n");
        printf("coop: field %u guest entity", ac3_vblank_count());
        for (u32 i = 0; i < 0x58u; i += 4) printf(" %08X", MEM_R32(e + i));
        printf("\n");
    }
    *ctx = saved;             /* the host's results (v0/v1) stand */

    /* swap the host back */
    guest_hud_swap();
    guest_camera_swap(host, mine);
    MEM_W32(slots, host);
    MEM_W32(slots + 4, mine);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(pp_addr(i), G.mine[i], per_player[i].len);
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_in(ho_addr(i), G.keep[i], host_only[i].len);
    G.updates++;
}

/* ---- the guest's view (spike S3) -------------------------------------------
 * On the guest's computer the frame's camera + drawing part runs with the
 * guest swapped in. Before it, RAM, scratchpad and the GTE are saved; after
 * it, everything is put back except the picture buffer the frame drew into
 * (the GPU draws it at the start of the next frame). The simulation therefore
 * stays identical to the host's; only the picture differs. */
extern u32 ac3_guest_ra;
extern u8 *ac3_scratch;

static int draw_buffer(u32 *lo, u32 *hi) {
    u32 w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(w)) return 0;
    u32 ot = MEM_R32(w + 0x10u), ot0 = MEM_R32(w + 0x18u), ot1 = MEM_R32(w + 0x1Cu);
    u32 pk0 = MEM_R32(w + 0x20u), pk1 = MEM_R32(w + 0x24u);
    if (!ram_ptr(ot0) || !ram_ptr(ot1) || !ram_ptr(pk0) || !ram_ptr(pk1) || ot1 <= pk0) return 0;
    u32 pk_size = ot1 - pk0;                 /* the two buffers sit back to back */
    if (ot == ot0) { *lo = ot0; *hi = pk0 + pk_size; }
    else if (ot == ot1) { *lo = ot1; *hi = pk1 + pk_size; }
    else return 0;
    return *hi > *lo && ((*hi - 0x80000000u) <= AC3_RAM_SIZE) && *hi - *lo <= 0x40000u;
}

extern void ac3_interpret_until(ctx_t *ctx, u32 pc, u32 stop);

/* Drawing packets the guest's picture uses outside its buffer (the HUD keeps
 * its own): found by walking the ordering table like the GPU will, copied
 * out before the host's memory is put back and copied in afterwards. */
#define MAX_EXTRA_BYTES 0x20000u
static struct { u32 addr, len; } extra[4096];
static unsigned n_extra;
static u8 extra_bytes[MAX_EXTRA_BYTES];

static void collect_packets(u32 lo, u32 hi, u32 ot_base, u32 ot_entries) {
    n_extra = 0;
    u32 used = 0, steps = 0;
    u32 a = ot_base + (ot_entries - 1u) * 4u;          /* ClearOTagR: drawn from the last entry */
    while (steps++ < 400000u) {
        if ((a >> 24) != 0x80u || (a & AC3_RAM_MASK) + 4u > AC3_RAM_SIZE) break;
        u32 word = MEM_R32(a), len = (word >> 24) * 4u + 4u;
        if ((a < lo || a >= hi) && n_extra < sizeof extra / sizeof extra[0] && used + len <= MAX_EXTRA_BYTES) {
            extra[n_extra].addr = a;
            extra[n_extra].len = len;
            memcpy(extra_bytes + used, ac3_ram + (a & AC3_RAM_MASK), len);
            used += len;
            n_extra++;
        }
        if ((word & 0xFFFFFFu) == 0xFFFFFFu) break;    /* end of the list */
        a = 0x80000000u | (word & 0xFFFFFFu);
    }
}

/* The guest's picture never enters the game's memory: it is laid over it
 * only while the GPU reads the frame (DrawOTag at the start of the next
 * frame), then the game's own bytes are put back. So both computers' game
 * memory stays byte-identical; only what reaches the screen differs. */
static struct { int ready, active; u32 lo, hi; } OV;
static u8 extra_backup[MAX_EXTRA_BYTES];
static u8 *ov_backup;

static void apply_overlay(void) {
    u8 *ram = ac3_ram;
    memcpy(ov_backup, ram + (OV.lo & AC3_RAM_MASK), OV.hi - OV.lo);
    memcpy(ram + (OV.lo & AC3_RAM_MASK), G.keep_buf, OV.hi - OV.lo);
    u32 used = 0;
    for (unsigned i = 0; i < n_extra; i++) {
        u8 *at = ram + (extra[i].addr & AC3_RAM_MASK);
        memcpy(extra_backup + used, at, extra[i].len);
        memcpy(at, extra_bytes + used, extra[i].len);
        used += extra[i].len;
    }
}

int ac3_coop_gpu_ready(void) { return ac3_coop_view && OV.ready && ov_backup; }

int ac3_coop_gpu_draw(int begin) {
    if (begin) {
        if (!ac3_coop_view || !OV.ready || !ov_backup) return 0;
        apply_overlay();
        OV.active = 1;
        return 1;
    } else if (OV.active) {
        /* in reverse order, so overlapping packets come back right */
        u32 used = 0;
        for (unsigned i = 0; i < n_extra; i++) used += extra[i].len;
        for (unsigned i = n_extra; i-- > 0; ) {
            used -= extra[i].len;
            memcpy(ac3_ram + (extra[i].addr & AC3_RAM_MASK), extra_backup + used, extra[i].len);
        }
        memcpy(ac3_ram + (OV.lo & AC3_RAM_MASK), ov_backup, OV.hi - OV.lo);
        OV.active = 0;
        OV.ready = 0;
    }
    return 0;
}

/* The guest's computer keeps two video memories. The game's own (ac3_vram)
 * receives exactly the host's picture, so whatever the game reads back from
 * the screen is the same on both computers. The display copy receives the
 * guest's picture and is what gets shown. */
static u16 *disp_vram;
static u32 disp_field;
#define VRAM_BYTES (1024u * 512u * 2u)

u16 *ac3_coop_display_pass(const u16 *game_vram) {
    int fresh = disp_vram != NULL && ac3_vblank_count() - disp_field <= 4u;
    if (!disp_vram) disp_vram = (u16 *)malloc(VRAM_BYTES);
    if (!disp_vram) return NULL;
    /* Bring textures and everything else up to date from the game's VRAM,
     * except the picture on screen right now: the game double-buffers, so
     * that is the guest's previous picture (this pass draws the other one). */
    gpu_display_t d;
    gpu_display_state(&d);
    int keep = fresh && d.enabled && d.w > 0 && d.h > 0;
    if (d.w > 1024) d.w = 1024;
    if (d.h > 512) d.h = 512;
    for (int y = 0; y < 512; y++) {
        u16 *dst = disp_vram + y * 1024;
        const u16 *src = game_vram + y * 1024;
        int in_rows = keep && ((y - d.y) & 511) < d.h;
        if (!in_rows) { memcpy(dst, src, 1024 * 2); continue; }
        int x0 = d.x & 1023, w = d.rgb24 ? (d.w * 3 + 1) / 2 : d.w;
        for (int x = 0; x < 1024; x++)
            if (((x - x0) & 1023) >= w) dst[x] = src[x];
    }
    disp_field = ac3_vblank_count();
    if (coop_debug > 0 && G.views < 4) printf("coop: guest picture drawn into the display VRAM at field %u\n", disp_field);
    return disp_vram;
}

u16 *ac3_coop_display_vram(u16 *game_vram) {
    /* the guest's picture is shown while it is fresh (the game draws one
     * picture per 1 or 2 fields); otherwise the game's own */
    if (disp_vram && ac3_coop_view && ac3_vblank_count() - disp_field <= 2u) return disp_vram;
    return game_vram;
}

/* Right after the player update (host and guest done): remember registers
 * and RAM. The rest of the frame (world, HUD preparation, camera, drawing)
 * runs normally for the host; the guest's view replays it from here. */
static void view_mark(ctx_t *ctx, u32 resume_pc) {
    G.view_on = 0;
    if (!G.live) return;
    if (!G.snap_ram) G.snap_ram = (u8 *)malloc(AC3_RAM_SIZE);
    if (!G.snap_ram) return;
    memcpy(G.snap_ram, ac3_ram, AC3_RAM_SIZE);
    memcpy(G.snap_scratch, ac3_scratch, sizeof G.snap_scratch);
    G.snap_ctx = *ctx;
    G.snap_pc = resume_pc;
    G.view_on = 1;
}

/* After the host's last drawing call: the frame's memory is now exactly the
 * host's. Replay the rest of the frame for the guest from the saved state
 * (the tick after the player update, camera, drawing), keep what the guest's
 * HUD decided (lock-on, both computers) and the picture it drew (guest's
 * computer only), then put the host's memory back. */
static void view_replay(ctx_t *ctx) {
    if (!G.view_on) return;
    G.view_on = 0;
    u32 lo = 0, hi = 0;
    if (!draw_buffer(&lo, &hi)) return;
    static u8 *host_ram;
    static u8 host_scratch[1024];
    if (!host_ram) host_ram = (u8 *)malloc(AC3_RAM_SIZE);
    if (!G.keep_buf) G.keep_buf = (u8 *)malloc(0x40000u);
    if (!host_ram || !G.keep_buf) return;
    memcpy(host_ram, ac3_ram, AC3_RAM_SIZE);
    memcpy(host_scratch, ac3_scratch, sizeof host_scratch);
    ctx_t host_ctx = *ctx;

    /* back to just after the player update, with the guest swapped in */
    memcpy(ac3_ram, G.snap_ram, AC3_RAM_SIZE);
    memcpy(ac3_scratch, G.snap_scratch, sizeof G.snap_scratch);
    *ctx = G.snap_ctx;
    u32 slots = MEM_R32(SLOT_TABLE), w = MEM_R32(WORLD_PTR);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) copy_in(pp_addr(i), G.mine[i], per_player[i].len);
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    MEM_W32(slots, mine);
    MEM_W32(slots + 4, host);
    if (G.cam_valid)
        for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) copy_in(w + view_private[i].off, G.cam[i], view_private[i].len);
    guest_hud_swap();         /* the guest's lock-on progress in (this RAM is thrown away after) */
    /* The camera follows "the player" through pointers into the host's entity
     * (world+0xEC -> its position): point them at the guest's entity. */
    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++)
        for (u32 o = 0; o + 4 <= view_private[i].len; o += 4) {
            u32 a = w + view_private[i].off + o, v = MEM_R32(a);
            if (v >= host && v < host + SLOT_SIZE) MEM_W32(a, mine + (v - host));
        }

    /* the rest of the tick (it returns to the interpreter's sentinel, because
     * the flight frame calls it through a pointer), then the flight frame
     * from just after that call to the end of drawing */
    G.replaying = 1;
    ac3_interpret_until(ctx, G.snap_pc, 0);
    ctx->r[31] = AFTER_TICK;
    ac3_interpret_until(ctx, AFTER_TICK, DRAW_LAST_RA);
    G.replaying = 0;

    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) copy_out(w + view_private[i].off, G.cam[i], view_private[i].len);
    G.cam_valid = 1;
    guest_hud_swap();         /* ... and out, with this frame's progress */
    /* The HUD code decides lock-on while it draws (0x8003F2CC writes the
     * locked target to 0x800C6D72, plus 0x800C6D68..71); the next frame's
     * player update and targeting read it. So the replay runs on both
     * computers and these bytes become part of the guest's state. */
    for (u32 a = 0x800C6D68u; a <= 0x800C6D72u; a++) G.mine[PP_TARGETING][a - 0x800C6D60u] = (u8)MEM_R8U(a);
    if (!ac3_coop_view) {                       /* host's computer: no picture to keep */
        memcpy(ac3_ram, host_ram, AC3_RAM_SIZE);
        memcpy(ac3_scratch, host_scratch, sizeof host_scratch);
        *ctx = host_ctx;
        G.views++;
        return;
    }
    u32 glo = 0, ghi = 0;
    int same = draw_buffer(&glo, &ghi) && glo == lo && ghi == hi;
    if (same) {
        memcpy(G.keep_buf, ac3_ram + (lo & AC3_RAM_MASK), hi - lo);
        u32 w2 = MEM_R32(WORLD_PTR), ot0 = MEM_R32(w2 + 0x18u), pk0 = MEM_R32(w2 + 0x20u);
        collect_packets(lo, hi, lo, (pk0 - ot0) / 4u);   /* both buffers have the same OT size */
    }

    /* the host's memory and registers again, with the guest's picture */
    memcpy(ac3_ram, host_ram, AC3_RAM_SIZE);
    memcpy(ac3_scratch, host_scratch, sizeof host_scratch);
    *ctx = host_ctx;
    if (!ov_backup) ov_backup = (u8 *)malloc(0x40000u);
    OV.ready = same && ov_backup;
    OV.lo = lo; OV.hi = hi;
    if (coop_debug > 0 && G.views < 3)
        printf("coop: guest view drawn at field %u (buffer %08X..%08X%s, %u packets outside it)\n",
               ac3_vblank_count(), lo, hi, same ? "" : ", NOT kept", n_extra);
    G.views++;
}

int ac3_coop_replaying(void) { return G.replaying; }

/* Once per field (psyq_etc.c). A mission is over when
 *   - the slot table moves (menus and briefings use their own), or
 *   - the player update has not run for 2 seconds and the host's player mode
 *     is no longer 1 (results, mission failed; a pause keeps mode 1), or
 *   - the host's player mode goes back to 2 (the next mission's intro).
 * The guest's aircraft goes away with it, and a guest that was destroyed may
 * fly again in the next mission. */
void ac3_coop_field(void) {
    if (!ac3_coop_guest) return;
    u32 mode_ptr = MEM_R32(GAME_GP + 0x520u);
    u32 mode = ram_ptr(mode_ptr) ? MEM_R32(mode_ptr + 4) : 0;
    int left = MEM_R32(SLOT_TABLE) != G.flight_table ||
               (ac3_vblank_count() - G.last_update_field > 120u && mode != 1u);
    int restart = mode == 2u && G.last_mode != 2u;
    G.last_mode = mode;
    if (!(G.live || G.dead) || !(left || restart)) return;
    if (G.live && ram_ptr(G.flight_table)) {
        u32 mine = MEM_R32(G.flight_table + 4);
        if (ram_ptr(mine) && MEM_R32(mine) == G.handle) MEM_W32(mine, G.handle & ~LIVE);   /* free slot 1 */
    }
    printf("coop: mission over at field %u (%s); guest aircraft %s\n", ac3_vblank_count(),
           left ? "left flight" : "next mission starting", G.live ? "removed" : "may fly again");
    G.live = 0;
    G.dead = 0;
}

int ac3_coop_intercept(ctx_t *ctx, u32 target) {
    if (!G.replaying && target == DRAW_LAST_FN && ac3_guest_ra == DRAW_LAST_RA && G.view_on) {
        ac3_interpret(ctx, target);
        view_replay(ctx);
        return 1;
    }
    if (target != PLAYER_UPDATE || G.replaying) return 0;
    u32 resume = ac3_guest_ra;           /* where the tick continues */
    ctx_t entry = *ctx;
    ac3_interpret(ctx, target);          /* the host, as always */
    ctx_t host_result = *ctx;
    guest_update(ctx, &entry);
    *ctx = host_result;
    view_mark(ctx, resume);              /* both computers: see view_replay */
    return 1;
}

void ac3_coop_status(char *out, int cap) {
    snprintf(out, (size_t)cap, "guest %s handle 0x%05X created %u updates %u views %u deaths %u",
             G.live ? "live" : G.dead ? "destroyed" : "none", G.handle, G.created_field, G.updates, G.views, G.deaths);
}
