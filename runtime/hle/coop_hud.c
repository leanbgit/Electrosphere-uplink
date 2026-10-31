/* coop_hud.c -- the HUD, per player.
 *
 * The HUD's state is worked out relative to "the player" and much of it is
 * kept from one frame to the next, so each player needs their own copy:
 * swapped in for the guest's update and the guest's view replay. Also here:
 * warnings, missiles coming at each player, and each player seeing the other
 * on the radar and on screen. */
#include "coop_internal.h"

/* ---- HUD state per player ---------------------------------------------------
 * The HUD's targets, seen from one player. Each target has a 0x34-byte
 * record (pointers at 0x800C67A0 + 4 * slot) filled relative to "the player":
 * the tick (0x80045278) stores the target's distance at +0x10 and only
 * recomputes it every 16 frames for targets over 0x84D0 away, and sets the
 * in-range/visible flags at +0x0C from it; the targeting step writes +0x2C;
 * the HUD shows and locks only targets flagged in range. Locking also takes
 * several frames: the HUD code (0x8003F2CC, 0x800409D4, 0x80040A18) keeps a
 * state and a timer in the first two bytes of the record and the lock in
 * progress at HUD_LOCK (state, target slot, and the lock box's shrinking
 * animation, 0x800409D4). With the host's copy, targets near
 * the guest but far from the host stayed "out of range" (the box flickered
 * and jumped) and a lock never completed.
 *
 * Other HUD state the drawing code keeps from one frame to the next, seen from
 * one player (0x8003D1A4, 0x8003DA70, 0x8003DD08, 0x8003D450, 0x800309A8:
 * per-target arrays; 0x800662B4: the gun reticle; the warning on screen): */
const coop_range_t coop_hud_static[N_HUD_STATIC] = {
    { HUD_ARRAYS,  0x480u },   /* 12 bytes x 32, then RADAR_BLIPS (12 bytes x 64) */
    { RADAR_RANGE - 4u, 0x08u },   /* incl. RADAR_RANGE (0x8003D450) */
    { HUD_MISC,    0x14u },
    /* the gun reticle (0x800662B4): a ring of 32 positions of the player's
     * bullets, one per frame, and its index; the reticle is drawn where the
     * bullets fired some frames ago have got to */
    { RETICLE_RING, 0x200u },
    { RETICLE_RING_INDEX, 0x04u },
    /* The warning on screen (0x80042544, from the HUD preparation 0x8003A0B4):
     * each frame it picks the most urgent of the player's warnings (missile
     * alert, lock-on, off course, altitude, PULL UP, autopilot, ...) and
     * FN_SHOW_WARNING keeps it at WARN_STATE until it runs out; a more
     * urgent one replaces it. */
    [HUD_WARN] = { WARN_STATE, 0x10u },
    { LOW_FRAMES,  0x01u },    /* frames too low, PULL UP after 30 */
};

static u32 msg_state(void) {
    u32 m = MEM_R32(MESSAGE_PTR);
    return ram_ptr(m) ? m + MSG_STATE : 0u;
}

/* Exchange the HUD state with the other player's copy (which starts as a copy
 * of the host's, the first time in a mission). The message on screen
 * (MESSAGE_PTR, see below) is part of it. */
void coop_hud_swap(void) {
    u32 ms = msg_state();
    if (!G.hud_valid) {
        if (ms) copy_out(ms, G.msg, MSG_STATE_LEN);
        copy_out(HUD_LOCK, G.hud_lock, HUD_LOCK_LEN);
        for (unsigned i = 0; i < N_HUD_STATIC; i++) copy_out(coop_hud_static[i].addr, G.hud_static[i], coop_hud_static[i].len);
        for (u32 t = 0; t < N_SLOTS; t++) {
            u32 r = MEM_R32(HUD_RECS + 4u * t);
            if (ram_ptr(r)) copy_out(r, G.hud_rec[t], HREC_SIZE);
        }
        G.hud_valid = 1;
    }
    exchange(HUD_LOCK, G.hud_lock, HUD_LOCK_LEN);
    if (ms) exchange(ms, G.msg, MSG_STATE_LEN);
    for (unsigned i = 0; i < N_HUD_STATIC; i++) exchange(coop_hud_static[i].addr, G.hud_static[i], coop_hud_static[i].len);
    for (u32 t = 0; t < N_SLOTS; t++) {
        u32 r = MEM_R32(HUD_RECS + 4u * t);
        if (ram_ptr(r)) exchange(r, G.hud_rec[t], HREC_SIZE);
    }
}

/* ---- missiles coming ----------------------------------------------------------
 * The missile manager (MSL_*) keeps, per target slot, who has missiles
 * coming (FN_MISSILE_LAUNCH fills it in when a missile is launched at a
 * handle). "The player" reads slot 0's: the targeting step (0x8003ADB4:
 * missile close, ALERT_MISSILE_CLOSE, the MISSILE warning) and the HUD
 * (0x8003DCF0, 0x80067144). While the guest is the player (its update, its
 * view replay) slots 0 and 1 are exchanged here too. */
void coop_threat_swap(void) {
    u32 t = MEM_R32(MISSILES_PTR);
    if (!ram_ptr(t)) return;
    u32 w = MEM_R32(t + MSL_TARGETED);
    MEM_W32(t + MSL_TARGETED, (w & ~3u) | (w & 1u) << 1 | (w >> 1 & 1u));
    static const u32 bytes[] = { MSL_LATEST, MSL_FIRED_BY, MSL_HOMING };
    for (unsigned i = 0; i < 3; i++) {
        u32 a = t + bytes[i], x = MEM_R8U(a), y = MEM_R8U(a + 1u);
        MEM_W8(a, y); MEM_W8(a + 1u, x);
    }
}

/* ---- warnings raised by the mission ----------------------------------------
 * The warning picker (FN_PICK_WARNING) runs in each player's HUD preparation,
 * so each player gets their own. Warnings the mission rules raise themselves
 * (FN_SHOW_WARNING called from outside the picker, e.g. FN_MISSION_RULES)
 * are shown to both: raised once more on the guest's copy. (Lock-on alerts:
 * coop_enemies.c.) */
static int warn_both(ctx_t *ctx) {
    int from_picker = ac3_guest_ra >= FN_PICK_WARNING && ac3_guest_ra < FN_SHOW_WARNING;   /* the picker's own calls */
    if (!G.live || G.replaying || !G.hud_valid || from_picker) return 0;
    ctx_t entry = *ctx;
    ac3_interpret(ctx, FN_SHOW_WARNING);
    ctx_t after = *ctx;
    *ctx = entry;
    exchange(WARN_STATE, G.hud_static[HUD_WARN], coop_hud_static[HUD_WARN].len);
    ac3_interpret(ctx, FN_SHOW_WARNING);
    exchange(WARN_STATE, G.hud_static[HUD_WARN], coop_hud_static[HUD_WARN].len);
    *ctx = after;
    return 1;
}

/* ---- messages: BINGO, BULLS EYE, MISSION ACCOMPLISHED, ... --------------------
 * FN_SHOW_MESSAGE puts the big text in the middle of the screen; its state
 * (MESSAGE_PTR) is ticked and drawn in the flight frame's drawing, so each
 * player keeps a copy (coop_hud_swap): the guest's is in during its update and
 * its view replay. Who sees a message:
 *   - the mission's (ENGAGE, MISSION ACCOMPLISHED, ...): both. Raised once
 *     more on the guest's copy, without the sound the first one played; the
 *     replay, which goes through the same code again, leaves them alone.
 *   - HOLD YOUR FIRE comes from each player's own HUD code: as it runs.
 *   - a kill (BINGO, BULLS EYE): the one who shot it down. The game shows it
 *     when an aircraft (FN_AI_FIRST) or ground object (FN_GROUND_DOWN) goes
 *     down and the last hit came from slot 0 (ENT_ATTACKER); the host's kills
 *     stay the host's. The guest's (last hit from slot 1) are found with the
 *     same checks, in the guest's view replay, and shown on the guest's copy
 *     after the game's own pass. */
static int is_hud_message(u32 ra)  { return ra == RA_HOLD_FIRE_1 || ra == RA_HOLD_FIRE_2; }
static int is_kill_message(u32 ra) { return ra == RA_KILL_AIRCRAFT || ra == RA_KILL_GROUND; }

static int show_message(ctx_t *ctx) {
    u32 ms = msg_state();
    if (!G.live || !G.hud_valid || !ms || is_hud_message(ac3_guest_ra)) return 0;
    if (G.replaying) return 1;                   /* shown to the guest already, or the host's kill */
    if (G.in_guest_update || is_kill_message(ac3_guest_ra)) return 0;   /* one player's own */
    ctx_t entry = *ctx;
    ac3_interpret(ctx, FN_SHOW_MESSAGE);
    ctx_t after = *ctx;
    *ctx = entry;
    exchange(ms, G.msg, MSG_STATE_LEN);
    G.quiet = 1;
    ac3_interpret(ctx, FN_SHOW_MESSAGE);
    G.quiet = 0;
    exchange(ms, G.msg, MSG_STATE_LEN);
    *ctx = after;
    return 1;
}

/* The kill message for e, if the guest shot it down (the game's own checks,
 * with slot 1 for slot 0); -1 if none. f: its SLOT_FLAGS word. */
static int guest_kill_message(u32 e, u32 f, int ground) {
    if (!ram_ptr(e) || !(f & SF_DOWN) || ent_health(e) > 0) return -1;
    if ((!ground && (f & SF_SCRIPTED)) || !(f & SF_KILL_MESSAGE)) return -1;
    if ((MEM_R32(e + ENT_ATTACKER) & 0x3Fu) != 1u) return -1;
    u32 w = MEM_R32(e + ENT_WORD), scene = MEM_R32(SCENE_PTR), world = MEM_R32(WORLD_PTR);
    if (w & ENT_NO_MESSAGE) return -1;
    s32 kind = MEM_R16S(e + ENT_KIND);
    if (ground) {
        if (ram_ptr(scene) && MEM_R16S(scene + SCENE_MISSION) == 0x16 && kind == 0x46) return MSG_TARGETED;
        return (w & ENT_MESSAGE) ? (int)MSG_BULLS_EYE : -1;
    }
    if (kind == 0x2C || !(w & ENT_MESSAGE) || !ram_ptr(world)) return -1;
    return (MEM_R32(world + WORLD_FRAME) & 7u) ? (int)MSG_BULLS_EYE : (int)MSG_BINGO;
}

/* (AC3_COOP_DEBUG) log each player's message as it changes */
void coop_message_log(void) {
    static u32 last[2] = { 0xFFu, 0xFFu };
    u32 ms = msg_state();
    if (coop_debug <= 0 || !ms || !G.hud_valid) return;
    u32 now[2] = { MEM_R32(ms + MSG_NUMBER - MSG_STATE), (u32)G.msg[MSG_NUMBER - MSG_STATE] | (u32)G.msg[MSG_NUMBER - MSG_STATE + 1] << 8 };
    for (int p = 0; p < 2; p++)
        if (now[p] != last[p]) {
            printf("coop: field %u %s message %u\n", ac3_vblank_count(), p ? "guest's" : "host's", now[p]);
            last[p] = now[p];
        }
}

int coop_kill_messages(ctx_t *ctx, u32 target) {
    if (!G.live || !G.replaying || !msg_state()) return 0;
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return 0;
    int found[N_SLOTS];
    unsigned n = 0;
    if (target == FN_AI_FIRST) {
        for (u32 s = SLOT_FIRST_AIRCRAFT; s < SLOT_FIRST_GROUND; s++) {
            int m = guest_kill_message(slot_entity(slots, s), MEM_R32(SLOT_FLAGS + 4u * s), 0);
            if (m >= 0) found[n++] = m;
        }
    } else {
        u32 i = ctx->r[4], list = MEM_R32(WEAPON_LIST);
        if (i >= N_SLOTS - SLOT_FIRST_GROUND || !ram_ptr(list)) return 0;
        int m = guest_kill_message(MEM_R32(list + 4u * i), MEM_R32(SLOT_FLAGS + 4u * (SLOT_FIRST_GROUND + i)), 1);
        if (m >= 0) found[n++] = m;
    }
    if (!n) return 0;
    ac3_interpret(ctx, target);
    ctx_t after = *ctx;
    for (unsigned k = 0; k < n; k++) call_guest(ctx, FN_SHOW_MESSAGE, (u32)found[k], 0, 0, 0);
    if (coop_debug > 0) printf("coop: field %u guest's kill: message %d\n", ac3_vblank_count(), found[0]);
    *ctx = after;
    return 1;
}

/* ---- the other player on the radar ------------------------------------------
 * The radar's blips (BLIP_* per slot at RADAR_BLIPS) are filled by 0x8003DA08
 * and drawn one by one by FN_DRAW_RADAR_BLIP from FN_DRAW_RADAR, both for
 * slots 2..63 only: neither player is on the other's radar. After the radar is
 * drawn, slot 1's (unused) entry is filled for the other player the way
 * 0x8003DA08 does it and drawn too, in yellow, so it stands apart from the
 * mission's allies. Each player's view does this for its own radar. */
static s32 div_trunc(s32 a, s32 b) { return b ? a / b : 0; }
static void radar_other_player(ctx_t *ctx) {
    u32 slots = MEM_R32(SLOT_TABLE), view = MEM_R32(RADAR_VIEW);
    if (!ram_ptr(slots) || !(ram_ptr(view) || (view >> 10) == (0x1F800000u >> 10))) return;   /* RAM or scratchpad */
    u32 me = slot_entity(slots, 0), e = slot_entity(slots, 1);
    if (!ram_ptr(me) || !ram_ptr(e) || !ent_live(e)) return;
    s32 range = (s32)MEM_R32(RADAR_RANGE);
    if (range <= 0) return;
    /* the radar's centre (c0, c1) and rotation (c2, c3 = sin, cos) */
    s32 c0 = (s32)MEM_R32(view), c1 = (s32)MEM_R32(view + 4u), c2 = (s32)MEM_R32(view + 8u), c3 = (s32)MEM_R32(view + 0xCu);
    s32 dx = (ent_x(e) >> 12) - c0, dz = c1 - (ent_z(e) >> 12);
    s32 x = div_trunc((s32)(((s64)dx * c3 + (s64)dz * c2) >> 12) << 5, range);
    s32 y = div_trunc((s32)(((s64)-dx * c2 + (s64)dz * c3) >> 12) << 5, range);
    if ((u32)(x + 0x1F) >= 0x3Fu || (u32)(y + 0x1F) >= 0x3Fu) return;   /* off the radar */
    u32 b = RADAR_BLIPS + 1u * BLIP_SIZE;                  /* slot 1's blip, unused by the game */
    u8 old[BLIP_SIZE];
    copy_out(b, old, BLIP_SIZE);
    for (u32 i = 0; i < BLIP_SIZE; i++) MEM_W8(b + i, 0);
    MEM_W8(b + BLIP_FLAGS, 0x7FFF < range ? 9u : 1u);
    if (MEM_R8U(RADAR_MODE) == 2u) MEM_W8(b + BLIP_FLAGS, MEM_R8U(b + BLIP_FLAGS) | 4u);
    MEM_W8(b + BLIP_KIND, BLIP_YELLOW);                    /* not one of the mission's allies */
    s32 dy = (ent_y(me) - ent_y(e)) >> 12;
    MEM_W8(b + BLIP_LEVEL, dy > 0x1000 ? 2u : dy >= -0x1000 ? 1u : 0u);
    MEM_W8(b + BLIP_HEADING, (u32)(((s32)MEM_R16S(e + ENT_HEADING) - (s32)MEM_R16S(me + ENT_HEADING) + 0x200) & 0x3FFF) >> 10);
    MEM_W8(b + BLIP_X, (u32)(x + 0x2C));                   /* the radar's centre is at (0x2C, 0x2E) */
    MEM_W8(b + BLIP_Y, (u32)(y + 0x2E));
    MEM_W32(b + BLIP_DIST, 0xFFFFFFFFu);
    ctx_t saved = *ctx;
    call_guest(ctx, FN_DRAW_RADAR_BLIP, b, 0, 0, 0);
    *ctx = saved;
    copy_in(b, old, BLIP_SIZE);
}

/* ---- the other player's FRND label on screen ---------------------------------
 * The game labels its targets from their HUD records: FN_PROJECT_TARGETS
 * projects slots 2..63 onto the screen (record +6 size, +8/+0xA position) and
 * FN_DRAW_LABELS draws the labels (for allies the pilot's name, 0x8004084C,
 * and FRND under it, FN_DRAW_LABEL(LABEL_FRND, left, bottom)). The players
 * have no record. So right after FN_PROJECT_TARGETS (the GTE still holds its
 * camera matrix) the other player is projected the same way, and after
 * FN_DRAW_LABELS a FRND label is drawn under it (no name: the name table only
 * holds the mission's characters). */
static struct { int on; s16 x, y, size; s32 dist; } MK;   /* where the other player is on screen */

static u32 isqrt64(double d) {
    u64 lo = 0, hi = 1u << 20;
    while (lo < hi) { u64 mid = (lo + hi + 1) / 2; if ((double)mid * mid <= d) lo = mid; else hi = mid - 1; }
    return (u32)lo;
}
static void project_other_player(ctx_t *ctx) {
    MK.on = 0;
    u32 slots = MEM_R32(SLOT_TABLE), w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(slots) || !ram_ptr(w)) return;
    u32 e = slot_entity(slots, 1), me = slot_entity(slots, 0), cam = MEM_R32(w + WORLD_CAMERA);
    if (!ram_ptr(e) || !ram_ptr(me) || !ram_ptr(cam) || !ent_live(e)) return;
    s32 v[3];                                                  /* from the camera, in world units */
    for (int c = 0; c < 3; c++) {
        v[c] = (ent_coord(e, c) - (s32)MEM_R32(cam + CAMOBJ_POS + 4u * (u32)c)) >> 12;
        if (v[c] < -0x8000 || v[c] > 0x7FFF) return;           /* too far, as FN_PROJECT_TARGETS */
    }
    u32 sp = ctx->r[29], buf = (sp - 0x40u) & ~7u;             /* scratch below the caller's stack */
    MEM_W16(buf + 0, (u32)v[0]); MEM_W16(buf + 2, (u32)v[1]); MEM_W16(buf + 4, (u32)v[2]); MEM_W16(buf + 6, 0);
    ctx_t saved = *ctx;
    ctx->r[29] = buf - 0x20u;
    call_guest(ctx, FN_ROTTRANSPERS, buf, buf + 8u, buf + 0x10u, buf + 0x14u);   /* (&v, &sxy, &p, &flag) */
    u32 otz = ctx->r[2];
    s32 flag = (s32)MEM_R32(buf + 0x14u);
    s32 sx = (s16)MEM_R16U(buf + 8u), sy = (s16)MEM_R16U(buf + 0xAu);
    call_guest(ctx, FN_BOX_SIZE, otz, 0, 0, 0);
    s32 size = (s16)ctx->r[2];
    *ctx = saved;
    if (flag < 0 || (u32)(sx & 0xFFFF) >= 0x141u || sy < 0 || sy >= 0xF1) return;   /* behind or off the 320x240 screen */
    s32 dx = (s32)((u32)ent_x(e) - (u32)ent_x(me)) >> 12, dy = (s32)((u32)ent_y(e) - (u32)ent_y(me)) >> 12,
        dz = (s32)((u32)ent_z(e) - (u32)ent_z(me)) >> 12;
    MK.on = 1; MK.x = (s16)sx; MK.y = (s16)sy; MK.size = (s16)size;
    MK.dist = (s32)isqrt64((double)dx * dx + (double)dy * dy + (double)dz * dz);
    if (coop_debug > 0) { static u32 n; if (n++ % 60u == 0) printf("coop: field %u other player on screen at %d,%d size %d dist %d%s\n",
        ac3_vblank_count(), sx, sy, size, MK.dist, G.replaying ? " (guest view)" : " (host view)"); }
}

/* run the game's function, then fn(ctx) with the game's registers kept */
static int run_then(ctx_t *ctx, u32 target, void (*fn)(ctx_t *)) {
    ac3_interpret(ctx, target);
    ctx_t after = *ctx;
    fn(ctx);
    *ctx = after;
    return 1;
}
/* The FRND label, and the blue box the game draws round allies. The box
 * routine (FN_DRAW_BOX) reads the slot's HUD record, and slot 1 has none:
 * it is given one for the call, on the stack, with what the game puts in an
 * ally's (shown, whole box, size and centre), then taken away again. */
static void label_other_player(ctx_t *ctx) {
    if (!MK.on) return;
    call_guest(ctx, FN_DRAW_LABEL, LABEL_FRND, (u32)(s32)(MK.x - MK.size), (u32)(s32)(MK.y + MK.size), 0);
    u32 m = MEM_R32(MISSION_STATE);
    if (ram_ptr(m) && (MEM_R32(m) & 4u)) return;              /* the game draws no boxes then either */
    u32 rec = (ctx->r[29] - 0x80u) & ~7u;
    for (u32 i = 0; i < HREC_SIZE; i += 4) MEM_W32(rec + i, 0);
    MEM_W8(rec + HREC_SHOWN, 1);
    MEM_W16(rec + HREC_BOX_SIZE, (u32)MK.size);
    MEM_W16(rec + HREC_SCREEN_X, (u32)MK.x);
    MEM_W16(rec + HREC_SCREEN_Y, (u32)MK.y);
    MEM_W32(rec + HREC_FLAGS, HREC_FULL_BOX | HREC_NEAR);
    u32 old_rec = MEM_R32(HUD_RECS + 4u), old_c = MEM_R32(HUD_RECS_C + 4u);
    MEM_W32(HUD_RECS + 4u, rec);
    MEM_W32(HUD_RECS_C + 4u, rec + HREC_FLAGS);
    ctx->r[29] = rec - 0x20u;
    call_guest(ctx, FN_DRAW_BOX, 1, COLOUR_FRND, 0, 0);
    MEM_W32(HUD_RECS + 4u, old_rec);
    MEM_W32(HUD_RECS_C + 4u, old_c);
    for (u32 i = 0; i < HREC_SIZE; i += 4) MEM_W32(rec + i, 0);
}

int coop_hud_hook(ctx_t *ctx, u32 target) {
    switch (target) {
    case FN_SHOW_WARNING:    return warn_both(ctx);
    case FN_SHOW_MESSAGE:    return show_message(ctx);
    case FN_MSG_VOICE:
    case FN_MSG_SOUND_STOP:
    case FN_MSG_SOUND:       return G.quiet;
    case FN_PROJECT_TARGETS: return G.live && run_then(ctx, target, project_other_player);
    case FN_DRAW_LABELS:     return G.live && run_then(ctx, target, label_other_player);
    case FN_DRAW_RADAR:      return G.live && run_then(ctx, target, radar_other_player);
    }
    return 0;
}
