/* coop_guest.c -- the guest's aircraft (docs/coop/S2_guest.md).
 *
 * The guest is a second copy of the player's aircraft in slot 1. Each frame,
 * right after the game's player update (0x800729DC) has run for the host, the
 * guest is swapped in as "the player" (slot 0, its copy of the player's
 * memory, its camera and HUD state), its controls are read from port 2, the
 * player update and the targeting step run again, and the host is swapped
 * back. */
#include "coop_internal.h"

/* Per-player memory: the guest has its own copy, exchanged with the host's
 * around the guest's update. */
const coop_range_t coop_per_player[N_PER_PLAYER] = {
    [PP_BLOCK]     = { 0,               BLOCK_SIZE },  /* aircraft state: where BLOCK_PTR points (pp_addr) */
    [PP_INPUT_MAP] = { INPUT_MAP,       0x40u },       /* incl. the 4 bytes at map+0x3C */
    [PP_POS_COPY]  = { POSITION_COPY,   0x10u },
    [PP_TARGETING] = { TARGETING_STATE, 0x18u },       /* FN_TARGETING's */
    [PP_ALERT]     = { ALERT_STATE,     0x10u },       /* incl. the lock-on / missile alert bits */
    [PP_WEAPONS]   = { SCENE + SCENE_WEAPONS, SCENE_WEAPONS_LEN },   /* the hangar's weapon choice */
    [PP_PLANE]     = { SCENE + SCENE_PLANE, 2u },     /* the hangar's plane (effects read it every frame) */
};

/* The guest's own plane and weapons (until the guest has its own pass
 * through the hangar: set by the page or the bench, --coop-guest-plane N,
 * --coop-guest-weapons G,M,S). */
u8  ac3_coop_guest_weapons[3];
int ac3_coop_guest_plane = -1;

/* What mission start does with the hangar's choice (FN_PLAYER_SETUP,
 * 0x8006CF90..0x8006D034), done again for the guest, which starts as a copy
 * of the host: its aircraft type (ENT_KIND) from PLANE_TYPES, its plane's
 * figures (FN_PLANE_FIGURES) into its copy of the block, its health, and the
 * salvo figure from its missile choice. The rest of that setup only resets
 * the flight state, which the copy already has. The plane's model, textures
 * and sounds stay the host's (step C, docs/coop/S4_plane_choice.md). */
static void guest_loadout(ctx_t *ctx, u32 mine) {
    int plane = ac3_coop_guest_plane >= 0 && ac3_coop_guest_plane <= 36;
    int weapons = ac3_coop_guest_weapons[0] || ac3_coop_guest_weapons[1] || ac3_coop_guest_weapons[2];
    if (!plane && !weapons) return;
    for (u32 i = 0; i < SCENE_WEAPONS_LEN; i++)
        if (ac3_coop_guest_weapons[i]) G.mine[PP_WEAPONS][i] = ac3_coop_guest_weapons[i];
    u32 cam = MEM_R32(CAMERA_STATE_PTR), o = cam - pp_addr(PP_BLOCK);
    if (!ram_ptr(cam) || o > BLOCK_SIZE - (CAMST_MISSILES + 4u)) return;
    u8 *c = &G.mine[PP_BLOCK][o];                    /* the guest's camera/weapon state */
    #define PUT32(off, v) do { u32 v_ = (u32)(v); for (u32 b_ = 0; b_ < 4; b_++) c[(off) + b_] = (u8)(v_ >> (8 * b_)); } while (0)
    u32 stats = (u32)c[CAMST_STATS] | (u32)c[CAMST_STATS + 1] << 8 | (u32)c[CAMST_STATS + 2] << 16 | (u32)c[CAMST_STATS + 3] << 24;
    if (plane) {
        G.mine[PP_PLANE][0] = (u8)ac3_coop_guest_plane;
        G.mine[PP_PLANE][1] = 0;
        u32 type = MEM_R8U(PLANE_TYPES + (u32)ac3_coop_guest_plane);
        MEM_W16(mine + ENT_KIND, type);
        ctx_t saved = *ctx;
        call_guest(ctx, FN_PLANE_FIGURES, type, 0, 0, 0);
        stats = ctx->r[2];
        *ctx = saved;
        if (!ram_ptr(stats)) return;
        u32 scene = MEM_R32(SCENE_PTR);
        s32 health = (s32)MEM_R32(stats + PLANE_HEALTH);
        if (ram_ptr(scene) && (s32)MEM_R32(scene + SCENE_TWO_PLACES) >= 2) health = 30;
        PUT32(CAMST_STATS, stats);
        PUT32(CAMST_FIG_08, MEM_R32(stats + PLANE_FIG_74));
        PUT32(CAMST_FIG_0C, MEM_R16S(stats + PLANE_FIG_80));
        PUT32(CAMST_FIG_10, MEM_R16S(stats + PLANE_FIG_82));
        PUT32(CAMST_HEALTH, health);
        PUT32(CAMST_MISSILES, MEM_R32(stats + PLANE_MISSILES));
        MEM_W16(mine + ENT_HEALTH, (u32)health);
    }
    if (ram_ptr(stats)) {
        s32 salvo = MEM_R16S(stats + PLANE_SALVO), limit = MEM_R16S(WEAPON_LIMITS + 2u * G.mine[PP_WEAPONS][1]);
        PUT32(CAMST_SALVO, limit < salvo ? limit : salvo);
    }
    #undef PUT32
    printf("coop: guest's plane %u (type %u), weapons %u,%u,%u; host's plane %u, weapons %u,%u,%u\n",
           G.mine[PP_PLANE][0], MEM_R16U(mine + ENT_KIND), G.mine[PP_WEAPONS][0], G.mine[PP_WEAPONS][1], G.mine[PP_WEAPONS][2],
           MEM_R16U(SCENE + SCENE_PLANE), MEM_R8U(SCENE + SCENE_WEAPONS), MEM_R8U(SCENE + SCENE_WEAPONS + 1),
           MEM_R8U(SCENE + SCENE_WEAPONS + 2));
}

/* Host-only memory the player update also writes: the guest's writes are
 * discarded. world = 1: an offset from the world pointer. */
const struct coop_host_only coop_host_only[N_HOST_ONLY] = {
    { ENGINE_SOUND,                0x40u, 0 },
    { LIBRARY_WORD,                0x04u, 0 },
    /* written when "the player" is shot down (0x80073880..0x800738C4): */
    { SCENE + SCENE_SHOT_DOWN,     0x02u, 0 },
    { SHOT_DOWN_EFFECT,            0x04u, 0 },
    { WORLD_SHAKE,                 0x08u, 1 },   /* (0x8006CBA4) */
    { WORLD_FLAGS,                 0x04u, 1 },   /* bit 0 hides the HUD (0x80035740, from the shot-down camera 0x80079564) */
};
static u32 ho_addr(unsigned i) { return coop_host_only[i].world ? MEM_R32(WORLD_PTR) + coop_host_only[i].addr : coop_host_only[i].addr; }

/* World-structure ranges only the camera and view setup write (relative to
 * the world pointer): the guest's view keeps its own copy between frames. */
const coop_range_t coop_view_private[N_VIEW_PRIVATE] = {
    { WORLD_VIEW_SETUP, 0x34u },
    { WORLD_CAMERA_OBJ, 0xE8u },    /* (0x8007B438, and 0x800793C4 when switching camera mode) */
};

/* Create the guest as a copy of the host's aircraft in slot 1, beside it. */
static int create_guest(ctx_t *ctx, u32 slots) {
    u32 host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    if (!ram_ptr(host) || !ram_ptr(mine)) return 0;
    u32 h = MEM_R32(host + ENT_WORD);
    if (!(h & ENT_LIVE) || ent_live(mine)) return 0;          /* no host, or slot 1 taken */
    /* Wait until the host has control (player mode 1). Before that the mission
     * moves the host's mode along (2 -> 4 -> 1) from outside the player
     * update, and a guest copied too early would stay in the intro mode. */
    u32 mode = MEM_R32(PLAYER_MODE_PTR);
    if (!ram_ptr(mode) || MEM_R32(mode + MODE_STATE) != 1u) return 0;
    u8 e[ENT_SIZE];
    copy_out(host, e, ENT_SIZE);
    copy_in(mine, e, ENT_SIZE);
    G.handle = (h & ~0x3Fu) | 1u;                             /* the host's handle, slot 1 */
    MEM_W32(mine + ENT_WORD, G.handle);
    MEM_W32(mine + ENT_X, (u32)ent_x(mine) + (150u << 12));   /* 150 units to the side */
    for (unsigned i = 0; i < N_PER_PLAYER; i++) copy_out(pp_addr(i), G.mine[i], coop_per_player[i].len);
    coop_hangar_flight();
    guest_loadout(ctx, mine);
    coop_plane_guest_created(ctx);
    G.live = 1;
    G.cam_valid = 0;          /* the world (and its camera) moves between missions */
    G.hud_valid = 0;
    G.shake_valid = 0;
    memset(G.flash, 0, sizeof G.flash);       /* no weapon-fire flash */
    G.ai_guest_group = 0;
    G.flight_table = slots;
    G.created_field = ac3_vblank_count();
    G.updates = 0;
    printf("coop: guest aircraft created in slot 1 (handle 0x%05X) at field %u\n", G.handle, G.created_field);
    return 1;
}

/* The flight tick's targeting step, for the guest (the tick runs it for the
 * host after the world update; 0x80032F3C):
 *     cam = *CAMERA_STATE_PTR;  args = { cam[0x1C], cam[8], cam[0xC], cam[0x10], out, target }
 *     FN_TARGETING(&args);     cam[CAMST_TARGET] = args.target   (the lock-on the launch code uses)
 * The camera part of the result (view+0xBC) is the view's business: skipped. */
static void guest_targeting(ctx_t *ctx) {
    u32 cam = MEM_R32(CAMERA_STATE_PTR);
    if (!ram_ptr(cam)) return;
    u32 args = (ctx->r[29] - 0x80u) & ~7u;     /* scratch below the caller's stack */
    MEM_W32(args + 0x00, MEM_R32(cam + 0x1Cu));
    MEM_W32(args + 0x04, MEM_R32(cam + 0x08u));
    MEM_W32(args + 0x08, MEM_R32(cam + 0x0Cu));
    MEM_W32(args + 0x0C, MEM_R32(cam + 0x10u));
    MEM_W32(args + 0x10, MEM_R32(cam + CAMST_TARGET));   /* not read before being written; keeps it tidy */
    MEM_W32(args + 0x14, MEM_R32(cam + CAMST_TARGET));
    u32 sp = ctx->r[29];
    ctx->r[29] = args - 0x10u;                  /* the tick's frame: args at sp+0x10 */
    call_guest(ctx, FN_TARGETING, args, 0, 0, 0);
    ctx->r[29] = sp;
    if (coop_debug > 0 && G.updates % 30u == 0)
        printf("coop: field %u guest targeting in %08X %08X %08X %08X -> out %08X target %08X\n", ac3_vblank_count(),
               MEM_R32(args), MEM_R32(args + 4), MEM_R32(args + 8), MEM_R32(args + 12), MEM_R32(args + 0x10), MEM_R32(args + 0x14));
    MEM_W32(cam + CAMST_TARGET, MEM_R32(args + 0x14));
}

/* Exchange the camera ranges of the world structure with the guest's copy.
 * The first time in a mission the guest's copy starts as the host's, pointed
 * at the guest's entity. */
static void guest_camera_swap(u32 host, u32 mine) {
    u32 w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(w)) return;
    if (!G.cam_valid) {
        for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) {
            copy_out(w + coop_view_private[i].addr, G.cam[i], coop_view_private[i].len);
            for (u32 o = 0; o + 4 <= coop_view_private[i].len; o += 4) {
                u8 *b = G.cam[i] + o;
                u32 v = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
                if (v >= host && v < host + ENT_SIZE) {
                    v = mine + (v - host);
                    b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24);
                }
            }
        }
        G.cam_valid = 1;
    }
    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) exchange(w + coop_view_private[i].addr, G.cam[i], coop_view_private[i].len);
}

static void debug_dump(u32 slots) {
    u32 e = slot_entity(slots, 0);   /* still the guest's entity here */
    printf("coop: field %u guest missiles %u (host %u)\n", ac3_vblank_count(), MEM_R16U(pp_addr(PP_BLOCK) + BLOCK_MISSILES),
           (u32)(G.mine[PP_BLOCK][BLOCK_MISSILES] | G.mine[PP_BLOCK][BLOCK_MISSILES + 1] << 8));
    printf("coop: field %u guest block", ac3_vblank_count());
    for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(pp_addr(PP_BLOCK) + i));
    printf("\n");
    printf("coop: field %u guest entity", ac3_vblank_count());
    for (u32 i = 0; i < ENT_SIZE; i += 4) printf(" %08X", MEM_R32(e + i));
    printf("\n");
}

static void guest_update(ctx_t *ctx, const ctx_t *entry) {
    if (coop_debug < 0) { const char *e = getenv("AC3_COOP_DEBUG"); coop_debug = e && *e && *e != '0'; }
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return;
    u32 host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    if (!ram_ptr(host) || !ram_ptr(mine) || !ram_ptr(pp_addr(PP_BLOCK))) return;
    G.last_update_field = ac3_vblank_count();

    /* Destroyed (crash, shot down): the game cleared its live bit. The guest
     * watches the host until the next mission. */
    if (G.live && !ent_live(mine)) {
        printf("coop: guest aircraft destroyed at field %u\n", ac3_vblank_count());
        G.live = 0;
        G.dead = 1;
        G.deaths++;
    }
    if (G.live && (MEM_R32(mine + ENT_WORD) & ~ENT_LIVE) != (G.handle & ~ENT_LIVE)) {   /* slot 1 taken over (not seen so far) */
        printf("coop: slot 1 reused by the game at field %u (0x%05X)\n", ac3_vblank_count(), MEM_R32(mine));
        G.live = 0;
    }
    if (!G.live && (G.dead || !create_guest(ctx, slots))) return;

    /* swap in the guest */
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_out(ho_addr(i), G.keep[i], coop_host_only[i].len);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(pp_addr(i), G.mine[i], coop_per_player[i].len);
    MEM_W32(slots, mine);
    MEM_W32(slots + 4, host);

    /* The player update also writes the camera: when "the player" is shot
     * down it switches it to follow the explosion (0x80077878 writes the
     * camera object). So the guest's update works on the guest's camera too. */
    guest_camera_swap(host, mine);
    coop_hud_swap();
    coop_threat_swap();
    coop_flash_swap();
    /* The screen shake (WORLD_SHAKE, host-only) is cleared each frame and
     * added to by the player update (FN_SHAKE_ADD: speed, damage). The guest's
     * starts from what it was before the host's update; its view replay uses
     * the guest's result (coop_view.c). */
    u32 world = MEM_R32(WORLD_PTR);
    int shake = ram_ptr(world);
    if (shake) copy_in(world + WORLD_SHAKE, G.shake_base, 8);
    G.in_guest_update = 1;
    u32 object = coop_plane_object_in();

    ctx_t saved = *ctx;
    call_guest(ctx, FN_INPUT_MAP, CTRL_PORT2, INPUT_MAP, INPUT_MAP + 0x3Cu, 0);
    if (coop_debug && G.updates % 30u == 0) {
        printf("coop: field %u guest map", ac3_vblank_count());
        for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(INPUT_MAP + i));
        printf("\n");
    }
    if (!getenv("AC3_COOP_NOUPDATE")) {     /* research switch: create the guest but never update it */
        call_guest(ctx, FN_PLAYER_UPDATE, entry->r[4], entry->r[5], entry->r[6], entry->r[7]);
        guest_targeting(ctx);
        coop_guest_hazards(ctx, mine);
    }
    if (coop_debug && G.updates % 30u == 0) debug_dump(slots);
    *ctx = saved;             /* the host's results (v0/v1) stand */
    G.in_guest_update = 0;
    coop_plane_object_out(object);
    if (shake) copy_out(world + WORLD_SHAKE, G.shake, 8);
    G.shake_valid = shake;

    /* swap the host back */
    coop_flash_swap();
    coop_threat_swap();
    coop_hud_swap();
    guest_camera_swap(host, mine);
    MEM_W32(slots, host);
    MEM_W32(slots + 4, mine);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(pp_addr(i), G.mine[i], coop_per_player[i].len);
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_in(ho_addr(i), G.keep[i], coop_host_only[i].len);
    G.updates++;
}

/* The game's player update: the host's as always, then the guest's, then a
 * snapshot for the guest's view (both computers: see coop_view.c). */
int coop_player_update(ctx_t *ctx) {
    if (G.replaying) return 0;
    u32 resume = ac3_guest_ra;           /* where the tick continues */
    ctx_t entry = *ctx;
    coop_prof_frame();
    u32 world = MEM_R32(WORLD_PTR);
    if (ram_ptr(world)) copy_out(world + WORLD_SHAKE, G.shake_base, 8);   /* before either player's shake */
    u64 t0 = coop_now_ns();
    ac3_interpret(ctx, FN_PLAYER_UPDATE);
    coop_prof_add(PROF_HOST_UPDATE, t0);
    ctx_t host_result = *ctx;
    u64 t1 = coop_now_ns();
    guest_update(ctx, &entry);
    coop_prof_add(PROF_GUEST_UPDATE, t1);
    *ctx = host_result;
    u64 t2 = coop_now_ns();
    coop_view_mark(ctx, resume);
    coop_prof_add(PROF_SNAPSHOT, t2);
    return 1;
}
