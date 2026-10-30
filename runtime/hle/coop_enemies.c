/* coop_enemies.c -- enemies treat the guest as a player.
 *
 * The enemy code takes "the player" to be slot 0 everywhere. To make enemies
 * notice, chase, lock on to and shoot the guest, the parts of their code that
 * should be about the guest run with the two players exchanged (the guest in
 * slot 0 and the host in slot 1, in the slot table and in the AI's tables),
 * and are put back afterwards. Both computers do exactly the same, so they
 * stay identical. */
#include "coop_internal.h"

static int players_exchanged;   /* an enemy's turn is running with slots 0 and 1 exchanged */
static int ground_vs_guest;     /* ... and it is a ground weapon aiming at the guest */

/* Exchange the two players everywhere the AI looks: their slots, their
 * attacker-table entries, and every 0/1 target reference. */
static void exchange_players(u32 slots, u32 recs) {
    u32 a = slot_entity(slots, 0), b = slot_entity(slots, 1);
    MEM_W32(slots, b); MEM_W32(slots + 4, a);
    u32 ta = MEM_R32(AI_TABLE), tb = MEM_R32(AI_TABLE + 4);
    MEM_W32(AI_TABLE, tb); MEM_W32(AI_TABLE + 4, ta);
    for (u32 t = 0; t < N_SLOTS; t++) MEM_W8(AI_TABLE + 4u * t + AIT_TARGET, flip01(MEM_R8U(AI_TABLE + 4u * t + AIT_TARGET)));
    for (u32 t = SLOT_FIRST_AIRCRAFT; t < SLOT_FIRST_GROUND; t++) {
        u32 rec = recs + AIREC_SIZE * t;
        MEM_W8(rec + AIREC_TARGET, flip01(MEM_R8U(rec + AIREC_TARGET)));
        MEM_W8(rec + AIREC_LAST_TARGET, flip01(MEM_R8U(rec + AIREC_LAST_TARGET)));
    }
}

/* ---- waking up ----------------------------------------------------------------
 * Which enemies the world keeps running. Each frame FN_ACTIVATE_AIRCRAFT
 * clears ENT_ACTIVE on aircraft slots 2..31 and sets it again by type; for
 * many (the bombers) only when their target record says HREC_NEAR (set by
 * 0x80045278 from the distance to the player, about 34,000 units).
 * FN_ACTIVATE_GROUND does the same for ground and ship objects (slots
 * 0x20..0x3F, WEAPON_LIST). The player is the host, so enemies near
 * only the guest stood still and SAM sites never fired at it. While these run,
 * a target near either player counts as near: the other player's records are
 * in G.hud_rec (the guest's in the game's own pass, the host's during the
 * guest's replay), so their bit is added and taken off again afterwards. */
static int activate_for_both(ctx_t *ctx, u32 fn) {
    if (!G.live || !G.hud_valid) return 0;
    int air = fn == FN_ACTIVATE_AIRCRAFT;
    u32 added[N_SLOTS] = {0}, lo = air ? SLOT_FIRST_AIRCRAFT : SLOT_FIRST_GROUND, hi = air ? SLOT_FIRST_GROUND : N_SLOTS;
    for (u32 t = lo; t < hi; t++) {
        u32 r = MEM_R32(HUD_RECS + 4u * t);
        if (!ram_ptr(r)) continue;
        const u8 *o = G.hud_rec[t] + HREC_FLAGS;
        u32 other = (u32)o[0] | (u32)o[1] << 8 | (u32)o[2] << 16 | (u32)o[3] << 24;
        u32 mine = MEM_R32(r + HREC_FLAGS);
        if ((other & HREC_NEAR) && !(mine & HREC_NEAR)) { MEM_W32(r + HREC_FLAGS, mine | HREC_NEAR); added[t] = 1; }
    }
    ac3_interpret(ctx, fn);
    for (u32 t = lo; t < hi; t++)
        if (added[t]) { u32 r = MEM_R32(HUD_RECS + 4u * t); MEM_W32(r + HREC_FLAGS, MEM_R32(r + HREC_FLAGS) & ~HREC_NEAR); }
    return 1;
}

/* ---- switching targets --------------------------------------------------------
 * An enemy keeps its target until the target is destroyed, and the ones that
 * hunt the player get the host at mission start, before the guest exists. So
 * an enemy attacking one player switches to the other when that one is much
 * nearer (under 60% of the distance) and has a free attacker place, moving its
 * place in the table as 0x800463B8/0x800464B0 do. Returns the new target. */
static u32 maybe_switch(u32 slots, u32 slot, u32 t, u32 e) {
    if (t > 1u) return t;
    u32 o = t ^ 1u, te = slot_entity(slots, t), oe = slot_entity(slots, o);
    if (!ram_ptr(te) || !ram_ptr(oe) || !ent_live(oe)) return t;
    s32 dt = dist_to(e, te), dn = dist_to(e, oe);
    if ((s64)dn * 10 >= (s64)dt * 6) return t;
    u32 to = AI_TABLE + 4u * o, tt = AI_TABLE + 4u * t;
    u32 place;
    if (MEM_R8U(to + AIT_ATTACKER1) == NO_SLOT) place = to + AIT_ATTACKER1;
    else if (MEM_R8U(to + AIT_ATTACKER2) == NO_SLOT && MEM_R32(MEM_R32(SCENE_PTR) + SCENE_TWO_PLACES) != 0) place = to + AIT_ATTACKER2;
    else return t;                                     /* the other player is busy enough */
    if (MEM_R8U(tt + AIT_ATTACKER1) == slot) MEM_W8(tt + AIT_ATTACKER1, NO_SLOT);
    if (MEM_R8U(tt + AIT_ATTACKER2) == slot) MEM_W8(tt + AIT_ATTACKER2, NO_SLOT);
    MEM_W8(place, slot);
    MEM_W8(AI_TABLE + 4u * slot + AIT_TARGET, o);
    return o;
}

/* ---- enemy aircraft -------------------------------------------------------------
 * The aircraft AI runs once per frame for all aircraft, in loops over slots
 * 2..31 that skip aircraft without both ENT_EXISTS and ENT_ACTIVE. Its code
 * takes "the player" to be slot 0 in many places: target choice (0x80046124
 * takes slot 0 when the player has a free attacker place in AI_TABLE;
 * otherwise the nearest opponent from slot 2 up, never the guest), scripted
 * "attack the player" (assigned target 0 at +0x1F of the AI record),
 * manoeuvres that fly to slot 0 (0x8004F570 mode 7, 0x80050BB4, 0x80050DE8),
 * aiming (0x8004AD98), and checks in the loops themselves. So each frame the
 * aircraft are split by "their" player (the one they attack, otherwise the
 * nearer one) and each loop runs twice: once for the host's aircraft as usual,
 * once for the guest's with the players exchanged. The aircraft of the other
 * group are marked inactive for that run, so each aircraft is handled once.
 * The groups are decided in FN_AI_DECIDE; FN_AI_PRE, which runs before it,
 * uses the previous frame's. */

/* run fn with the aircraft outside `group` marked inactive */
static void ai_run_group(ctx_t *ctx, u32 fn, u32 slots, u32 group) {
    u32 masked = 0;
    for (u32 t = SLOT_FIRST_AIRCRAFT; t < SLOT_FIRST_GROUND; t++) {
        if (group & (1u << t)) continue;
        u32 e = slot_entity(slots, t);
        if (ram_ptr(e) && (MEM_R32(e + ENT_WORD) & ENT_ACTIVE)) { MEM_W32(e + ENT_WORD, MEM_R32(e + ENT_WORD) & ~ENT_ACTIVE); masked |= 1u << t; }
    }
    ac3_interpret(ctx, fn);
    for (u32 t = SLOT_FIRST_AIRCRAFT; t < SLOT_FIRST_GROUND; t++)
        if (masked & (1u << t)) { u32 e = slot_entity(slots, t); MEM_W32(e + ENT_WORD, MEM_R32(e + ENT_WORD) | ENT_ACTIVE); }
}
/* this frame's groups: bit n = the aircraft in slot n is after the guest */
static void ai_decide_groups(u32 slots, u32 recs, u32 host, u32 mine) {
    G.ai_guest_group = 0;
    for (u32 t = SLOT_FIRST_AIRCRAFT; t < SLOT_FIRST_GROUND; t++) {
        u32 e = slot_entity(slots, t);
        if (!ram_ptr(e) || (~MEM_R32(e + ENT_WORD) & (ENT_ACTIVE | ENT_EXISTS))) continue;
        u32 rec = recs + AIREC_SIZE * t;
        u32 tg = MEM_R8U(rec + AIREC_TARGET), n = maybe_switch(slots, t, tg, e);
        if (n != tg) { MEM_W8(rec + AIREC_TARGET, n); MEM_W8(rec + AIREC_LAST_TARGET, n); tg = n; }
        int guest = tg == 1u ? 1 : tg == 0u ? 0 : dist_to(e, mine) < dist_to(e, host);
        if (guest) G.ai_guest_group |= 1u << t;
    }
}
static int ai_two_groups(ctx_t *ctx, u32 fn) {
    if (!G.live || G.replaying) return 0;
    u32 slots = MEM_R32(SLOT_TABLE), recs = MEM_R32(AI_RECS);
    u32 host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    if (!ram_ptr(recs) || !ram_ptr(host) || !ram_ptr(mine) || !ent_live(mine)) return 0;
    if (fn == FN_AI_DECIDE) ai_decide_groups(slots, recs, host, mine);
    if (!G.ai_guest_group) return 0;                /* all the host's: as in single player */
    ctx_t entry = *ctx;
    ai_run_group(ctx, fn, slots, ~G.ai_guest_group);
    *ctx = entry;
    exchange_players(slots, recs);
    players_exchanged = 1;
    ai_run_group(ctx, fn, slots, G.ai_guest_group);
    players_exchanged = 0;
    exchange_players(slots, recs);
    return 1;
}

/* ---- ground and ship weapons ------------------------------------------------------
 * FN_GROUND_WEAPON runs once per weapon i of WEAPON_LIST (its slot is 0x20 +
 * i, its target slot at WPN_TARGET). With no target, every 64 frames it asks
 * 0x80046124, which only ever offers slot 0 or a non-player opponent, and it
 * keeps the target until it is out of range (0x800464B0). Its range check
 * also differs: for "the player" it uses the distance in its target record
 * (HREC_DIST); for any other target it adds up |dx|+|dy|+|dz|, up to 1.7
 * times larger, so a SAM with range 6,000 dropped the guest again at once and
 * never fired.
 * So a weapon aiming at the guest, or with no target and the guest nearer,
 * runs as the guest's: the players exchanged, its target 0 instead of 1, and
 * the guest's copy of its target record in place. A weapon aiming at one
 * player switches to the other when that one is much nearer. */
static u32 ground_entity(u32 i) {
    u32 list = MEM_R32(WEAPON_LIST);
    return ram_ptr(list) && i < N_SLOTS - SLOT_FIRST_GROUND ? MEM_R32(list + 4u * i) : 0;
}
static void ground_maybe_switch(u32 i) {
    u32 slots = MEM_R32(SLOT_TABLE), e = ground_entity(i);
    if (!ram_ptr(slots) || !ram_ptr(e)) return;
    u32 t = MEM_R8U(e + WPN_TARGET);
    if (t <= 1u) MEM_W8(e + WPN_TARGET, maybe_switch(slots, SLOT_FIRST_GROUND + i, t, e));
}
static int ground_for_guest(ctx_t *ctx) {
    u32 i = ctx->r[4], slot = SLOT_FIRST_GROUND + i, slots = MEM_R32(SLOT_TABLE), recs = MEM_R32(AI_RECS), e = ground_entity(i);
    if (!ram_ptr(slots) || !ram_ptr(recs) || !ram_ptr(e) || !G.hud_valid) return 0;
    u32 t = MEM_R8U(e + WPN_TARGET), host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    if (!ram_ptr(host) || !ram_ptr(mine) || !ent_live(mine)) return 0;
    if (!(t == 1u || (t == NO_SLOT && dist_to(e, mine) < dist_to(e, host)))) return 0;   /* the host's: as usual */
    u32 rec = MEM_R32(HUD_RECS + 4u * slot);
    exchange_players(slots, recs);
    if (t == 1u) MEM_W8(e + WPN_TARGET, 0u);
    if (ram_ptr(rec)) exchange(rec, G.hud_rec[slot], HREC_SIZE);
    ground_vs_guest = players_exchanged = 1;
    ac3_interpret(ctx, FN_GROUND_WEAPON);
    ground_vs_guest = players_exchanged = 0;
    if (ram_ptr(rec)) exchange(rec, G.hud_rec[slot], HREC_SIZE);
    MEM_W8(e + WPN_TARGET, flip01(MEM_R8U(e + WPN_TARGET)));
    exchange_players(slots, recs);
    return 1;
}
static int ground_weapon(ctx_t *ctx) {
    if (!G.live || G.replaying) return 0;
    ground_maybe_switch(ctx->r[4]);
    return ground_for_guest(ctx);                   /* 0: runs as usual */
}

/* ---- missile launches ---------------------------------------------------------------
 * FN_MISSILE_LAUNCH (a3 = the target's handle) looks the target up in the
 * slot table by the handle's slot and keeps pointers to its position and
 * velocity (missile record +0x74, +0x78), which the missile then homes on. A handle is the real one
 * (the guest's is slot 1), so while an enemy's turn runs with the players
 * exchanged the lookup found the host's entity: the missile counted as the
 * guest's (warnings) but flew at the host's position. The slot table is put
 * straight for the launch. */
static int launch_real_slots(ctx_t *ctx) {
    if (!players_exchanged) return 0;
    u32 slots = MEM_R32(SLOT_TABLE), a = slot_entity(slots, 0), b = slot_entity(slots, 1);
    MEM_W32(slots, b); MEM_W32(slots + 4, a);
    ac3_interpret(ctx, FN_MISSILE_LAUNCH);
    MEM_W32(slots, a); MEM_W32(slots + 4, b);
    return 1;
}

/* ---- lock-on alerts -----------------------------------------------------------------
 * An enemy weapon (FN_LOCK_ALERT, for weapon i of WEAPON_LIST) raises
 * ALERT_LOCKED_ON half-way through its lock, whoever it aims at; the AI
 * dispatcher (0x8003809C) clears it each frame before FN_AI_FIRST. The alert
 * word is per-player memory: the bit now goes to the player aimed at (in the
 * main pass the host's copy is in RAM and the guest's in G.mine; in the
 * guest's view replay the guest's is in RAM). */
#define ALERT_WORD     (ALERT_STATE + ALERT_BITS)
static int lock_alert(ctx_t *ctx) {
    if (!G.live) return 0;
    u32 idx = ctx->r[4], obj = ground_entity(idx);
    u32 tgt = ram_ptr(obj) ? MEM_R8U(obj + WPN_TARGET) : NO_SLOT;
    u32 before = MEM_R32(ALERT_WORD);
    ac3_interpret(ctx, FN_LOCK_ALERT);
    u32 after = MEM_R32(ALERT_WORD);
    if (!(after & ~before & ALERT_LOCKED_ON)) return 1;
    u32 me = G.replaying ? 1u : 0u;                 /* whose copy is in RAM */
    if (ground_vs_guest && tgt <= 1u) tgt ^= 1u;    /* players exchanged for the call (ground_for_guest) */
    if (coop_debug > 0) printf("coop: field %u lock-on alert, weapon %u aims at slot %u%s\n", ac3_vblank_count(), idx, tgt,
                               G.replaying ? " (view replay)" : "");
    if (tgt == me) return 1;
    MEM_W32(ALERT_WORD, after & ~ALERT_LOCKED_ON);
    if (!G.replaying && tgt == 1u) G.mine[PP_ALERT][ALERT_BITS] |= ALERT_LOCKED_ON;
    return 1;
}
static int clear_guest_alert(void) {
    if (G.live && !G.replaying) G.mine[PP_ALERT][ALERT_BITS] &= (u8)~ALERT_LOCKED_ON;
    return 0;                                       /* then runs as usual */
}

int coop_enemies_hook(ctx_t *ctx, u32 target) {
    switch (target) {
    case FN_ACTIVATE_AIRCRAFT:
    case FN_ACTIVATE_GROUND: return activate_for_both(ctx, target);
    case FN_AI_PRE:
    case FN_AI_DECIDE:
    case FN_AI_MOVE:
    case FN_AI_WEAPON:       return ai_two_groups(ctx, target);
    case FN_AI_FIRST:        return clear_guest_alert();
    case FN_GROUND_WEAPON:   return ground_weapon(ctx);
    case FN_MISSILE_LAUNCH:  return launch_real_slots(ctx);
    case FN_LOCK_ALERT:      return lock_alert(ctx);
    }
    return 0;
}
