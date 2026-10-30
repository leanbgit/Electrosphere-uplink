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
#define PP_ALERT     4         /* +0xC = 0x800C6ECC: bit 0 "an enemy is locking on", bit 1 "missile close" */
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
static int ground_vs_guest;     /* a ground weapon aiming at the guest runs with the players exchanged */
static int players_exchanged;   /* slots 0 and 1 exchanged for an enemy's turn (see launch_real_slots) */
static u32 ai_guest_group;     /* enemy AI: bit n = aircraft in slot n is the guest's this frame */

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
#define WARN_STATE     0x800C7180u
#define HUD_WARN       5u        /* index of WARN_STATE in hud_static */
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
    /* The warning on screen (0x80042544, from the HUD preparation 0x8003A0B4):
     * each frame it picks the most urgent of the player's warnings (missile
     * alert, lock-on, off course, altitude, PULL UP, autopilot, ...) and
     * 0x8004287C keeps it at 0x800C7180 (number, time left, two animation
     * records) until it runs out; a more urgent one replaces it. Kept per
     * player, or the host's warning stayed on the guest's screen. */
    { WARN_STATE,  0x10u },
    { 0x800BCFC0u, 0x01u },    /* gp+0x44C: frames too low, PULL UP after 30 */
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
    ai_guest_group = 0;
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

/* Mission hazards that hurt "the player". The mission rules (0x800540E0, once
 * per frame) look at slot 0 only; most of what they check is a mission goal
 * or failure (see mission_rules_for_both), but in mission scene 0x2A flying
 * too low costs health: 0x80076FA4 takes a point off slot 0 (and shakes the
 * screen, host-only memory) when, with t = frame counter (world+0xC) & 31:
 *     height (+0xC) >= 0 and t == 0, > 0x01387FFF and t == 16,
 *     > 0x01F3FFFF and t == 8 or 24.
 * The guest gets the same, here, while it is swapped in as slot 0. */
#define SCENE_PTR      (GAME_GP + 0x2ECu)  /* scene; +0x38C: mission scene number */
#define HAZARD_FN      0x80076FA4u
static void guest_hazards(ctx_t *ctx, u32 e) {
    u32 scene = MEM_R32(SCENE_PTR), w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(scene) || !ram_ptr(w) || !(MEM_R32(e) & LIVE)) return;
    if (MEM_R16S(scene + 0x38Cu) != 0x2A || MEM_R32(w + 4u) != 4u || (MEM_R32(scene) & 0x20000000u)) return;
    s32 y = (s32)MEM_R32(e + 0xCu);
    u32 t = MEM_R32(w + 0xCu) & 0x1Fu;
    if ((y >= 0 && t == 0u) || (y > 0x01387FFF && t == 16u) || (y > 0x01F3FFFF && (t == 8u || t == 24u)))
        call_guest(ctx, HAZARD_FN, 0, 0, 0, 0);
}

/* The missile manager ([gp+0x50C]) keeps, per target slot, who has missiles
 * coming: word +0 bit n (slots 0..31), byte +8+n the latest missile at slot
 * n, +0x48+n missiles fired by n, +0x88+n missiles homing on n (0x800685C0
 * fills them in when a missile is launched at a handle). "The player" reads
 * slot 0's: the targeting step (0x8003ADB4: missile close, bit 1 of
 * 0x800C6ECC, the MISSILE warning) and the HUD (0x8003DCF0, 0x80067144). While
 * the guest is the player (its update, its view replay) slots 0 and 1 are
 * exchanged here too. */
#define MISSILES_PTR   (GAME_GP + 0x50Cu)
static void threat_swap(void) {
    u32 t = MEM_R32(MISSILES_PTR);
    if (!ram_ptr(t)) return;
    u32 w = MEM_R32(t);
    MEM_W32(t, (w & ~3u) | (w & 1u) << 1 | (w >> 1 & 1u));
    static const u32 bytes[] = { 0x08u, 0x48u, 0x88u };
    for (unsigned i = 0; i < 3; i++) {
        u32 a = t + bytes[i], x = MEM_R8U(a), y = MEM_R8U(a + 1u);
        MEM_W8(a, y); MEM_W8(a + 1u, x);
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
    threat_swap();

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
        guest_hazards(ctx, mine);
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
    threat_swap();
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
    threat_swap();            /* the guest's incoming missiles as "the player's" */
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

/* Which aircraft the world keeps running. Each frame 0x80038100 clears the
 * "active" bit (0x8000) of aircraft slots 2..31 and sets it again by type; for
 * many (the bombers) only when their target record says "near the player"
 * (bit 0x80 at +0x0C, set by 0x80045278 from the distance to the player, about
 * 34,000 units). The player is the host, so enemies near only the guest stood
 * still. While it runs, a target near either player counts as near: the other
 * player's records are in G.hud_rec (the guest's in the game's own pass, the
 * host's during the guest's replay), so their bit is added and taken off again
 * afterwards. Both computers do the same, so they stay identical.
 * 0x80051498 does the same for ground and ship objects (slots 0x20..0x3F, the
 * table at gp+0x2D8): SAM and missile sites near only the guest stayed asleep
 * and never fired at it (their update 0x80052FA8 skips inactive objects). */
#define ACTIVATE_FN    0x80038100u
#define ACTIVATE_GROUND_FN 0x80051498u   /* the same for ground and ship objects, slots 0x20..0x3F */
static void activate_for_both(ctx_t *ctx, u32 fn) {
    u32 added[64] = {0}, lo = fn == ACTIVATE_FN ? 2u : 0x20u, hi = fn == ACTIVATE_FN ? 32u : 64u;
    for (u32 t = lo; t < hi; t++) {
        u32 r = MEM_R32(HUD_RECS + 4u * t);
        if (!ram_ptr(r)) continue;
        const u8 *o = G.hud_rec[t] + 0x0Cu;
        u32 other = (u32)o[0] | (u32)o[1] << 8 | (u32)o[2] << 16 | (u32)o[3] << 24;
        u32 mine = MEM_R32(r + 0x0Cu);
        if ((other & 0x80u) && !(mine & 0x80u)) { MEM_W32(r + 0x0Cu, mine | 0x80u); added[t] = 1; }
    }
    ac3_interpret(ctx, fn);
    for (u32 t = lo; t < hi; t++)
        if (added[t]) { u32 r = MEM_R32(HUD_RECS + 4u * t); MEM_W32(r + 0x0Cu, MEM_R32(r + 0x0Cu) & ~0x80u); }
}

/* Enemy AI and "the player". The aircraft AI runs once per frame for all
 * aircraft: 0x800469EC (decisions) and 0x80046DFC (manoeuvres), both looping
 * over slots 2..31 and skipping aircraft without both 0x4000 and 0x8000. Its
 * code takes "the player" to be slot 0 in many places: target choice
 * (0x80046124 takes slot 0 when the player has a free attacker place in the
 * table at 0x800C6D80 + 4 * slot, +1 its target, +2/+3 its attackers; otherwise
 * the nearest opponent from slot 2 up, never the guest), scripted "attack the
 * player" (assigned target 0 at +0x1F of the AI record), manoeuvres that fly to
 * slot 0 (0x8004F570 mode 7, 0x80050BB4, 0x80050DE8), and checks in the loops
 * themselves (the same holds for 0x80046854 and the attack routines under
 * 0x8004701C). So each frame the aircraft are split by "their" player (the one
 * they attack, otherwise the nearer one) and each loop runs twice: once for the
 * host's aircraft as usual, once for the guest's with the two players
 * exchanged everywhere the AI looks (their slots, their table entries, every
 * aircraft's 0/1 references). The aircraft of the other group are marked
 * inactive (0x8000 cleared) for that run, so each aircraft is handled once. */
#define AI_DECIDE_FN   0x800469ECu         /* runs third; the groups are decided here */
#define AI_MOVE_FN     0x80046DFCu
#define AI_PRE_FN      0x80046854u         /* runs before 0x800469EC: uses the previous frame's groups */
#define AI_WEAPON_FN   0x8004701Cu         /* attacks (0x8004AD98 aims at slot 0) */
#define AI_TABLE       0x800C6D80u         /* 4 bytes per slot: +1 its target, +2/+3 its attackers */
#define AI_RECS        (GAME_GP + 0x34Cu)  /* 0xB8-byte AI record per slot: +0x60 target, +0x3F last target */
#define AI_ACTIVE      0x8000u

static s32 dist_to(u32 e, u32 o) {
    s32 d = 0;
    for (int c = 0; c < 3; c++) { s32 v = ((s32)MEM_R32(e + 8u + 4u * c) >> 12) - ((s32)MEM_R32(o + 8u + 4u * c) >> 12); d += v < 0 ? -v : v; }
    return d;
}
static u32 flip01(u32 v) { return v == 0u ? 1u : v == 1u ? 0u : v; }

/* An enemy keeps its target until the target is destroyed, and the ones that
 * hunt the player get the host at mission start, before the guest exists. So
 * an enemy attacking one player switches to the other when that one is much
 * nearer (under 60% of the distance) and has a free attacker place, moving its
 * place in the table as 0x800463B8/0x800464B0 do. */
static u32 ai_switch_core(u32 slots, u32 slot, u32 t, u32 e) {
    if (t > 1u) return t;
    u32 o = t ^ 1u, te = MEM_R32(slots + 4u * t), oe = MEM_R32(slots + 4u * o);
    if (!ram_ptr(te) || !ram_ptr(oe) || !(MEM_R32(oe) & LIVE)) return t;
    s32 dt = dist_to(e, te), dn = dist_to(e, oe);
    if ((s64)dn * 10 >= (s64)dt * 6) return t;
    u32 to = AI_TABLE + 4u * o, tt = AI_TABLE + 4u * t;
    u32 place;
    if (MEM_R8U(to + 2u) == 0xFFu) place = to + 2u;
    else if (MEM_R8U(to + 3u) == 0xFFu && MEM_R32(MEM_R32(GAME_GP + 0x2ECu) + 0x394u) != 0) place = to + 3u;
    else return t;                                     /* the other player is busy enough */
    if (MEM_R8U(tt + 2u) == slot) MEM_W8(tt + 2u, 0xFFu);
    if (MEM_R8U(tt + 3u) == slot) MEM_W8(tt + 3u, 0xFFu);
    MEM_W8(place, slot);
    MEM_W8(AI_TABLE + 4u * slot + 1u, o);
    return o;
}
static void ai_maybe_switch(u32 slots, u32 slot, u32 rec, u32 e) {
    u32 t = MEM_R8U(rec + 0x60u), n = ai_switch_core(slots, slot, t, e);
    if (n == t) return;
    MEM_W8(rec + 0x60u, n);
    MEM_W8(rec + 0x3Fu, n);
}


static void ai_exchange_players(u32 slots, u32 recs) {
    u32 a = MEM_R32(slots), b = MEM_R32(slots + 4);
    MEM_W32(slots, b); MEM_W32(slots + 4, a);
    u32 ta = MEM_R32(AI_TABLE), tb = MEM_R32(AI_TABLE + 4);
    MEM_W32(AI_TABLE, tb); MEM_W32(AI_TABLE + 4, ta);
    for (u32 t = 0; t < 64u; t++) MEM_W8(AI_TABLE + 4u * t + 1u, flip01(MEM_R8U(AI_TABLE + 4u * t + 1u)));
    for (u32 t = 2; t < 32u; t++) {
        u32 rec = recs + 0xB8u * t;
        MEM_W8(rec + 0x60u, flip01(MEM_R8U(rec + 0x60u)));
        MEM_W8(rec + 0x3Fu, flip01(MEM_R8U(rec + 0x3Fu)));
    }
}
/* run fn with the aircraft outside `group` marked inactive */
static void ai_run_group(ctx_t *ctx, u32 fn, u32 slots, u32 group) {
    u32 masked = 0;
    for (u32 t = 2; t < 32u; t++) {
        if (group & (1u << t)) continue;
        u32 e = MEM_R32(slots + 4u * t);
        if (ram_ptr(e) && (MEM_R32(e) & AI_ACTIVE)) { MEM_W32(e, MEM_R32(e) & ~AI_ACTIVE); masked |= 1u << t; }
    }
    ac3_interpret(ctx, fn);
    for (u32 t = 2; t < 32u; t++)
        if (masked & (1u << t)) { u32 e = MEM_R32(slots + 4u * t); MEM_W32(e, MEM_R32(e) | AI_ACTIVE); }
}
static int ai_two_groups(ctx_t *ctx, u32 fn) {
    u32 slots = MEM_R32(SLOT_TABLE), recs = MEM_R32(AI_RECS);
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(recs) || !ram_ptr(host) || !ram_ptr(mine) || !(MEM_R32(mine) & LIVE)) return 0;
    if (fn == AI_DECIDE_FN) {                       /* this frame's groups */
        ai_guest_group = 0;
        for (u32 t = 2; t < 32u; t++) {
            u32 e = MEM_R32(slots + 4u * t);
            if (!ram_ptr(e) || (~MEM_R32(e) & 0xC000u)) continue;
            u32 rec = recs + 0xB8u * t;
            ai_maybe_switch(slots, t, rec, e);
            u32 tg = MEM_R8U(rec + 0x60u);
            int guest = tg == 1u ? 1 : tg == 0u ? 0 : dist_to(e, mine) < dist_to(e, host);
            if (guest) ai_guest_group |= 1u << t;
        }
    }
    if (!ai_guest_group) return 0;                  /* all the host's: as in single player */
    ctx_t entry = *ctx;
    ai_run_group(ctx, fn, slots, ~ai_guest_group);
    *ctx = entry;
    ai_exchange_players(slots, recs);
    players_exchanged = 1;
    ai_run_group(ctx, fn, slots, ai_guest_group);
    players_exchanged = 0;
    ai_exchange_players(slots, recs);
    return 1;
}

/* Mission rules about where "the player" is. 0x800540E0 (once per frame,
 * from 0x800371C4) has, per mission scene (scene+0x38C), checks on slot 0's
 * position that move the mission on or fail it (radio messages through
 * 0x8004287C, mission scripts through 0x80054EBC, mission flags):
 *     0x0E, 0x35  height (+0xC) > 0xFB1E0000 (lower than that); in 0x0E,
 *                 while the mission flag 2 (gp+0x340) is set, the same check
 *                 only tints the screen: left to each player's own view
 *     0x06        height < 0xFF600000
 *     0x2C        height < 0xFFA00000, or x and z inside a box
 *     0x17        |x| and |z| <= 0x0946FFFF and height > 0xFC400000
 *     0x37        z past 0x11D28000 (0x343F0000 when scene+0x394 is set)
 * (0x2A's checks are damage: guest_hazards.) Both players must meet the
 * condition (climb to the height, reach the area, ...): when the host meets it
 * and the guest, still flying, does not, the rules run with the guest's
 * position in the host's entity for the call, so they see it unmet; the host's
 * position is put back afterwards unless the rules moved the host themselves.
 * A guest that was shot down no longer holds the team back. Everything else
 * the rules do stays the host's (story choices and route following are the
 * host's). */
#define MISSION_FN     0x800540E0u
#define MISSION_STATE  (GAME_GP + 0x340u)
static int mission_condition(s32 scene_no, u32 scene, u32 e) {
    s32 x = (s32)MEM_R32(e + 8u), y = (s32)MEM_R32(e + 0xCu), z = (s32)MEM_R32(e + 0x10u);
    u32 m = MEM_R32(MISSION_STATE);
    switch (scene_no) {
    case 0x0E: return y > (s32)0xFB1E0000 && !(ram_ptr(m) && (MEM_R32(m) & 2u));
    case 0x35: return y > (s32)0xFB1E0000;
    case 0x06: return y < (s32)0xFF600000;
    case 0x2C: return y < (s32)0xFFA00000 ||
                      ((u32)(x + 0x01F3FFFF) <= 0x07147FFEu && z > (s32)0xFB9B0000 && z <= 0x01387FFF);
    case 0x17: return (x < 0 ? -x : x) <= 0x0946FFFF && (z < 0 ? -z : z) <= 0x0946FFFF && y > (s32)0xFC400000;
    case 0x37: return MEM_R32(scene + 0x394u) ? z > 0x343F0000 : z > 0x11D28000;
    default:   return 0;
    }
}
static int mission_rules_for_both(ctx_t *ctx) {
    u32 slots = MEM_R32(SLOT_TABLE), scene = MEM_R32(SCENE_PTR);
    if (!ram_ptr(slots) || !ram_ptr(scene)) return 0;
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(host) || !ram_ptr(mine) || !(MEM_R32(mine) & LIVE)) return 0;
    s32 no = (s32)MEM_R16S(scene + 0x38Cu);
    if (!mission_condition(no, scene, host) || mission_condition(no, scene, mine)) return 0;
    u32 hp[3], gp3[3];
    for (u32 i = 0; i < 3u; i++) { hp[i] = MEM_R32(host + 8u + 4u * i); gp3[i] = MEM_R32(mine + 8u + 4u * i); }
    for (u32 i = 0; i < 3u; i++) MEM_W32(host + 8u + 4u * i, gp3[i]);
    ac3_interpret(ctx, MISSION_FN);
    int moved = 0;
    for (u32 i = 0; i < 3u; i++) moved |= MEM_R32(host + 8u + 4u * i) != gp3[i];
    if (!moved) for (u32 i = 0; i < 3u; i++) MEM_W32(host + 8u + 4u * i, hp[i]);
    if (coop_debug > 0) printf("coop: field %u mission scene 0x%02X rule met by the host but not the guest\n", ac3_vblank_count(), (u32)no);
    return 1;
}

/* Effects around "the player" (0x800850D8, every frame after the player
 * update): its vapour/wake (0x80082E80), the scene's weather around it at
 * speed (0x80084F6C, reads the speed in the aircraft block), scene 0x2A's
 * effect (0x80084E6C), and the flash 0x800831D0 called by 0x80090148. Each is
 * also run for the other player: slot 0 and 1 exchanged, the other player's
 * aircraft block in, the entity arguments pointed at the other entity. The
 * guest's view replay does the same the other way round (the host's block is
 * in the saved RAM then), so each view sees both planes' effects. */
static const struct { u32 fn, ra; int needs_health; } fx_calls[] = {
    { 0x80082E80u, 0x8008511Cu, 0 },
    { 0x80084F6Cu, 0x80085154u, 1 },
    { 0x80084E6Cu, 0x80085198u, 1 },
    { 0x800831D0u, 0x80090200u, 0 },
};
#define N_FX_CALLS (sizeof fx_calls / sizeof fx_calls[0])
static void other_block_swap(u8 *tmp, int first) {
    u32 pp = pp_addr(PP_BLOCK), n = per_player[PP_BLOCK].len;
    if (!G.replaying) { exchange(pp, G.mine[PP_BLOCK], n); return; }
    if (first) memcpy(tmp, G.snap_ram + (pp & AC3_RAM_MASK), n);
    exchange(pp, tmp, n);
}
static int fx_for_both(ctx_t *ctx, unsigned k) {
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return 0;
    u32 a = MEM_R32(slots), b = MEM_R32(slots + 4);
    if (!ram_ptr(a) || !ram_ptr(b) || !ram_ptr(pp_addr(PP_BLOCK)) || (G.replaying && !G.snap_ram)) return 0;
    ctx_t entry = *ctx;
    ac3_interpret(ctx, fx_calls[k].fn);
    ctx_t after = *ctx;
    if ((MEM_R32(b) & LIVE) && (!fx_calls[k].needs_health || (s32)MEM_R16S(b + 0x4Eu) > 0)) {
        static u32 n[N_FX_CALLS];
        if (coop_debug > 0 && n[k]++ % 600u == 0)
            printf("coop: field %u effect %08X for the other player (%u)%s\n", ac3_vblank_count(), fx_calls[k].fn, n[k],
                   G.replaying ? " in the view replay" : "");
        *ctx = entry;
        for (int r = 4; r <= 7; r++)
            if (ctx->r[r] >= a && ctx->r[r] < a + SLOT_SIZE) ctx->r[r] = b + (ctx->r[r] - a);
        u8 tmp[0x100];
        MEM_W32(slots, b); MEM_W32(slots + 4, a);
        other_block_swap(tmp, 1);
        ac3_interpret(ctx, fx_calls[k].fn);
        other_block_swap(tmp, 0);
        MEM_W32(slots, a); MEM_W32(slots + 4, b);
    }
    *ctx = after;
    return 1;
}

/* Warnings for the right player.
 * - Lock-on alert: an enemy weapon (0x800535D4, for weapon object a0 of the
 *   table at gp+0x2D8; its target slot at +0x5B) sets bit 0 of 0x800C6ECC
 *   half-way through its lock, whoever it aims at; the AI dispatcher
 *   (0x8003809C) clears it each frame before 0x80046600. 0x800C6ECC is
 *   per-player memory: the bit now goes to the player aimed at (in the main
 *   pass the host's copy is in RAM and the guest's in G.mine; in the guest's
 *   view replay the guest's is in RAM).
 * - Warnings the mission rules raise themselves (0x8004287C called from
 *   outside the warning picker 0x80042544, e.g. 0x800540E0): shown to both. */
#define LOCK_ALERT_FN  0x800535D4u
#define AI_FIRST_FN    0x80046600u
#define SHOW_WARN_FN   0x8004287Cu
#define WARN_PICK_FN   0x80042544u
static void lock_alert(ctx_t *ctx) {
    u32 list = MEM_R32(GAME_GP + 0x2D8u), alert = per_player[PP_ALERT].addr + 0xCu;
    u32 obj = ram_ptr(list) ? MEM_R32(list + 4u * ctx->r[4]) : 0;
    u32 tgt = ram_ptr(obj) ? MEM_R8U(obj + 0x5Bu) : 0xFFu, idx = ctx->r[4];
    u32 before = MEM_R32(alert);
    ac3_interpret(ctx, LOCK_ALERT_FN);
    u32 after = MEM_R32(alert);
    if (!(after & ~before & 1u)) return;
    u32 me = G.replaying ? 1u : 0u;          /* whose copy is in RAM */
    if (ground_vs_guest && tgt <= 1u) tgt ^= 1u;   /* players exchanged for the call (ground_for_guest) */
    if (coop_debug > 0) printf("coop: field %u lock-on alert, weapon %u aims at slot %u%s\n", ac3_vblank_count(), idx, tgt,
                               G.replaying ? " (view replay)" : "");
    if (tgt == me) return;
    MEM_W32(alert, after & ~1u);
    if (!G.replaying && tgt == 1u) G.mine[PP_ALERT][0xC] |= 1u;
}
static void warn_both(ctx_t *ctx) {
    ctx_t entry = *ctx;
    ac3_interpret(ctx, SHOW_WARN_FN);
    ctx_t after = *ctx;
    *ctx = entry;
    exchange(WARN_STATE, G.hud_static[HUD_WARN], 0x10u);
    ac3_interpret(ctx, SHOW_WARN_FN);
    exchange(WARN_STATE, G.hud_static[HUD_WARN], 0x10u);
    *ctx = after;
}

/* Ground and ship weapons (0x80052FA8, one call per weapon i of the table at
 * gp+0x2D8; its slot is 0x20 + i, its target slot at +0x5B, 0xFF = none).
 * With no target, every 64 frames it asks 0x80046124, which only ever offers
 * slot 0 ("the player", when it has a free attacker place) or a non-player
 * opponent: the guest was never chosen. It keeps the target until it is out
 * of range (0x800464B0). So a weapon with no target and the guest nearer, or
 * aiming at the guest, runs as if the guest were the player (ground_for_guest),
 * and a weapon aiming at one player switches to the other when that one is
 * much nearer (ai_switch_core). The target keeps its real slot number. */
#define GROUND_FN      0x80052FA8u
static u32 ground_entity(u32 i) {
    u32 list = MEM_R32(GAME_GP + 0x2D8u);
    return ram_ptr(list) && i < 0x20u ? MEM_R32(list + 4u * i) : 0;
}
static void ground_maybe_switch(u32 i) {
    u32 slots = MEM_R32(SLOT_TABLE), e = ground_entity(i);
    if (!ram_ptr(slots) || !ram_ptr(e)) return;
    u32 t = MEM_R8U(e + 0x5Bu);

    if (t <= 1u) MEM_W8(e + 0x5Bu, ai_switch_core(slots, 0x20u + i, t, e));
}
/* As the guest's: the two players exchanged (slots, attacker table), its
 * target 0 instead of 1, and the guest's copy of its target record in place.
 * For "the player" the weapon checks its range against the distance in that
 * record (0x800C6ED0 + 4 * slot points at record+0xC, the distance at +4); for
 * any other target it adds up |dx|+|dy|+|dz|, up to 1.7 times larger, so a SAM
 * with range 6,000 dropped the guest again at once and never fired. */
static int ground_for_guest(ctx_t *ctx) {
    u32 i = ctx->r[4], slot = 0x20u + i, slots = MEM_R32(SLOT_TABLE), recs = MEM_R32(AI_RECS), e = ground_entity(i);
    if (!ram_ptr(slots) || !ram_ptr(recs) || !ram_ptr(e) || !G.hud_valid) return 0;
    u32 t = MEM_R8U(e + 0x5Bu), host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(host) || !ram_ptr(mine) || !(MEM_R32(mine) & LIVE)) return 0;
    if (!(t == 1u || (t == 0xFFu && dist_to(e, mine) < dist_to(e, host)))) return 0;   /* the host's: as usual */
    u32 rec = MEM_R32(HUD_RECS + 4u * slot);
    ai_exchange_players(slots, recs);
    if (t == 1u) MEM_W8(e + 0x5Bu, 0u);
    if (ram_ptr(rec)) exchange(rec, G.hud_rec[slot], HUD_REC_LEN);
    ground_vs_guest = players_exchanged = 1;
    ac3_interpret(ctx, GROUND_FN);
    ground_vs_guest = players_exchanged = 0;
    if (ram_ptr(rec)) exchange(rec, G.hud_rec[slot], HUD_REC_LEN);
    MEM_W8(e + 0x5Bu, flip01(MEM_R8U(e + 0x5Bu)));
    ai_exchange_players(slots, recs);
    return 1;
}

/* Missile launch (0x800685C0, a3 = the target's handle). It looks the target
 * up in the slot table by the handle's slot and keeps pointers to its position
 * and velocity (+0x74, +0x78), which the missile then homes on. A handle is the
 * real one (the guest's is slot 1), so while an enemy's turn runs with the
 * players exchanged the lookup found the host's entity: the missile was counted
 * as the guest's (warnings) but flew at the host's position. The slot table is
 * put straight for the launch. */
#define MISSILE_LAUNCH_FN 0x800685C0u
static void launch_real_slots(ctx_t *ctx) {
    u32 slots = MEM_R32(SLOT_TABLE), a = MEM_R32(slots), b = MEM_R32(slots + 4);
    MEM_W32(slots, b); MEM_W32(slots + 4, a);
    ac3_interpret(ctx, MISSILE_LAUNCH_FN);
    MEM_W32(slots, a); MEM_W32(slots + 4, b);
}

int ac3_coop_intercept(ctx_t *ctx, u32 target) {
    if (players_exchanged && target == MISSILE_LAUNCH_FN) { launch_real_slots(ctx); return 1; }
    if (G.live && !G.replaying && target == GROUND_FN) {
        ground_maybe_switch(ctx->r[4]);
        if (ground_for_guest(ctx)) return 1;      /* otherwise runs as usual */
    }
    if (G.live && target == LOCK_ALERT_FN) { lock_alert(ctx); return 1; }
    if (G.live && !G.replaying && target == AI_FIRST_FN) G.mine[PP_ALERT][0xC] &= (u8)~1u;   /* then runs as usual */
    if (G.live && !G.replaying && G.hud_valid && target == SHOW_WARN_FN &&
        !(ac3_guest_ra >= WARN_PICK_FN && ac3_guest_ra < SHOW_WARN_FN)) { warn_both(ctx); return 1; }
    if (G.live && !G.replaying && target == MISSION_FN && mission_rules_for_both(ctx)) return 1;
    if (G.live)
        for (unsigned k = 0; k < N_FX_CALLS; k++)
            if (target == fx_calls[k].fn && ac3_guest_ra == fx_calls[k].ra) return fx_for_both(ctx, k);
    if ((target == ACTIVATE_FN || target == ACTIVATE_GROUND_FN) && G.live && G.hud_valid) {
        activate_for_both(ctx, target);
        return 1;
    }
    if (G.live && !G.replaying && (target == AI_DECIDE_FN || target == AI_MOVE_FN || target == AI_PRE_FN ||
                                   target == AI_WEAPON_FN) && ai_two_groups(ctx, target))
        return 1;
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
