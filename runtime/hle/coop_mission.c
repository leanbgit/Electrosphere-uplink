/* coop_mission.c -- the mission and the world around "the player".
 *
 * Mission rules that check where the player is, hazards that hurt the player,
 * and the effects drawn around the player's aircraft. (Story choices and
 * route following stay the host's.) */
#include "coop_internal.h"

/* ---- hazards ----------------------------------------------------------------
 * The mission rules (FN_MISSION_RULES, once per frame) look at slot 0 only;
 * in mission scene 0x2A flying too low costs health: FN_HAZARD_DAMAGE takes a
 * point off slot 0 (and shakes the screen, host-only memory) when, with t =
 * WORLD_FRAME & 31:
 *     height (+0xC) >= 0 and t == 0, > 0x01387FFF and t == 16,
 *     > 0x01F3FFFF and t == 8 or 24.
 * The guest gets the same during its own update, while it is slot 0. */
#define SCENE_LOW_DAMAGE 0x2A
void coop_guest_hazards(ctx_t *ctx, u32 e) {
    u32 scene = MEM_R32(SCENE_PTR), w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(scene) || !ram_ptr(w) || !ent_live(e)) return;
    if (MEM_R16S(scene + SCENE_MISSION) != SCENE_LOW_DAMAGE || MEM_R32(w + WORLD_STATE) != 4u ||
        (MEM_R32(scene + SCENE_FLAGS) & 0x20000000u)) return;
    s32 y = ent_y(e);
    u32 t = MEM_R32(w + WORLD_FRAME) & 0x1Fu;
    if ((y >= 0 && t == 0u) || (y > 0x01387FFF && t == 16u) || (y > 0x01F3FFFF && (t == 8u || t == 24u)))
        call_guest(ctx, FN_HAZARD_DAMAGE, 0, 0, 0, 0);
}

/* ---- mission rules about where the player is --------------------------------
 * FN_MISSION_RULES (once per frame, from 0x800371C4) has, per mission scene
 * (SCENE_MISSION), checks on slot 0's position that move the mission on or
 * fail it (warnings through FN_SHOW_WARNING, mission scripts through
 * 0x80054EBC, mission flags):
 *     0x0E, 0x35  height (+0xC) > 0xFB1E0000 (lower than that); in 0x0E,
 *                 while the mission flag 2 (gp+0x340) is set, the same check
 *                 only tints the screen: left to each player's own view
 *     0x06        height < 0xFF600000
 *     0x2C        height < 0xFFA00000, or x and z inside a box
 *     0x17        |x| and |z| <= 0x0946FFFF and height > 0xFC400000
 *     0x37        z past 0x11D28000 (0x343F0000 when scene+0x394 is set)
 * (0x2A's checks are damage: coop_guest_hazards.) Both players must meet the
 * condition (climb to the height, reach the area, ...): when the host meets it
 * and the guest, still flying, does not, the rules run with the guest's
 * position in the host's entity for the call, so they see it unmet; the host's
 * position is put back afterwards unless the rules moved the host themselves.
 * A guest that was shot down no longer holds the team back. Everything else
 * the rules do stays the host's. */
static int mission_condition(s32 scene_no, u32 scene, u32 e) {
    s32 x = ent_x(e), y = ent_y(e), z = ent_z(e);
    u32 m = MEM_R32(MISSION_STATE);
    switch (scene_no) {
    case 0x0E: return y > (s32)0xFB1E0000 && !(ram_ptr(m) && (MEM_R32(m) & 2u));
    case 0x35: return y > (s32)0xFB1E0000;
    case 0x06: return y < (s32)0xFF600000;
    case 0x2C: return y < (s32)0xFFA00000 ||
                      ((u32)(x + 0x01F3FFFF) <= 0x07147FFEu && z > (s32)0xFB9B0000 && z <= 0x01387FFF);
    case 0x17: return (x < 0 ? -x : x) <= 0x0946FFFF && (z < 0 ? -z : z) <= 0x0946FFFF && y > (s32)0xFC400000;
    case 0x37: return MEM_R32(scene + SCENE_TWO_PLACES) ? z > 0x343F0000 : z > 0x11D28000;
    default:   return 0;
    }
}
static int mission_rules_for_both(ctx_t *ctx) {
    if (!G.live || G.replaying) return 0;
    u32 slots = MEM_R32(SLOT_TABLE), scene = MEM_R32(SCENE_PTR);
    if (!ram_ptr(slots) || !ram_ptr(scene)) return 0;
    u32 host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    if (!ram_ptr(host) || !ram_ptr(mine) || !ent_live(mine)) return 0;
    s32 no = (s32)MEM_R16S(scene + SCENE_MISSION);
    if (!mission_condition(no, scene, host) || mission_condition(no, scene, mine)) return 0;
    s32 host_pos[3], guest_pos[3];
    for (int c = 0; c < 3; c++) { host_pos[c] = ent_coord(host, c); guest_pos[c] = ent_coord(mine, c); }
    for (int c = 0; c < 3; c++) MEM_W32(host + ENT_POS + 4u * (u32)c, (u32)guest_pos[c]);
    ac3_interpret(ctx, FN_MISSION_RULES);
    int moved = 0;
    for (int c = 0; c < 3; c++) moved |= ent_coord(host, c) != guest_pos[c];
    if (!moved) for (int c = 0; c < 3; c++) MEM_W32(host + ENT_POS + 4u * (u32)c, (u32)host_pos[c]);
    if (coop_debug > 0) printf("coop: field %u mission scene 0x%02X rule met by the host but not the guest\n", ac3_vblank_count(), (u32)no);
    return 1;
}

/* ---- effects around the player -----------------------------------------------
 * 0x800850D8 (every frame after the player update) makes the effects around
 * "the player": its vapour/wake (FN_FX_WAKE), the scene's weather around it
 * at speed (FN_FX_WEATHER, reads the speed in the aircraft block), scene
 * 0x2A's effect (FN_FX_SCENE2A), and a flash (FN_FX_FLASH). Each
 * call from there (recognised by its return address) is also run for the
 * other player: slots 0 and 1 exchanged, the other player's aircraft block in,
 * the entity arguments pointed at the other entity. The guest's view replay
 * does the same the other way round (the host's block is in the saved RAM
 * then), so each view sees both planes' effects. */
static const struct { u32 fn, ra; int needs_health; } fx_calls[] = {
    { FN_FX_WAKE,    RA_FX_WAKE,    0 },
    { FN_FX_WEATHER, RA_FX_WEATHER, 1 },
    { FN_FX_SCENE2A, RA_FX_SCENE2A, 1 },
    { FN_FX_FLASH,   RA_FX_FLASH,   0 },
};
#define N_FX_CALLS (sizeof fx_calls / sizeof fx_calls[0])
static void other_block_swap(u8 *tmp, int first) {
    u32 pp = pp_addr(PP_BLOCK), n = coop_per_player[PP_BLOCK].len;
    if (!G.replaying) { exchange(pp, G.mine[PP_BLOCK], n); return; }
    if (first) memcpy(tmp, G.snap_ram + (pp & AC3_RAM_MASK), n);
    exchange(pp, tmp, n);
}
static int fx_for_both(ctx_t *ctx, unsigned k) {
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return 0;
    u32 a = slot_entity(slots, 0), b = slot_entity(slots, 1);
    if (!ram_ptr(a) || !ram_ptr(b) || !ram_ptr(pp_addr(PP_BLOCK)) || (G.replaying && !G.snap_ram)) return 0;
    ctx_t entry = *ctx;
    ac3_interpret(ctx, fx_calls[k].fn);
    ctx_t after = *ctx;
    if (ent_live(b) && (!fx_calls[k].needs_health || ent_health(b) > 0)) {
        static u32 n[N_FX_CALLS];
        if (coop_debug > 0 && n[k]++ % 600u == 0)
            printf("coop: field %u effect %08X for the other player (%u)%s\n", ac3_vblank_count(), fx_calls[k].fn, n[k],
                   G.replaying ? " in the view replay" : "");
        *ctx = entry;
        for (int r = 4; r <= 7; r++)
            if (ctx->r[r] >= a && ctx->r[r] < a + ENT_SIZE) ctx->r[r] = b + (ctx->r[r] - a);   /* -> into the entity */
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

int coop_mission_hook(ctx_t *ctx, u32 target) {
    if (target == FN_MISSION_RULES) return mission_rules_for_both(ctx);
    if (!G.live) return 0;
    for (unsigned k = 0; k < N_FX_CALLS; k++)
        if (target == fx_calls[k].fn && ac3_guest_ra == fx_calls[k].ra) return fx_for_both(ctx, k);
    return 0;
}
