/* coop.c -- see coop.h; how the co-op code is organised: coop_internal.h.
 *
 * Every call the game's code makes goes through ac3_coop_intercept first (in
 * co-op builds). Each topic file has a hook that recognises the calls it cares
 * about and either handles them (returns 1: the game's function has been run,
 * usually wrapped in something) or lets the game run them as usual (0). */
#include "coop_internal.h"

int ac3_coop_guest;
int ac3_coop_view;
int ac3_coop_split;
int coop_debug = -1;
coop_state_t coop_g;

int ac3_coop_intercept(ctx_t *ctx, u32 target) {
    switch (target) {
    case FN_PLAYER_UPDATE: return coop_player_update(ctx);
    case FN_DRAW_LAST:     return coop_view_after_drawing(ctx);
    case FN_DRAW_TERRAIN:
    case FN_DRAW_WORLD:    return coop_view_skip_3d(target);
    }
    return coop_hud_hook(ctx, target) || coop_enemies_hook(ctx, target) || coop_mission_hook(ctx, target);
}

int ac3_coop_replaying(void) { return G.replaying; }

/* ---- timing ------------------------------------------------------------------ */
#include <time.h>
static int prof_on = -1;
static u64 prof_ns[N_PROF], prof_frames, prof_start;
u64 coop_now_ns(void) {
    if (prof_on < 0) { const char *e = getenv("AC3_COOP_PROF"); prof_on = e && *e && *e != '0'; }
    if (!prof_on) return 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}
void coop_prof_add(int what, u64 since) { if (prof_on > 0) prof_ns[what] += coop_now_ns() - since; }
void coop_prof_frame(void) {
    if (prof_on <= 0) return;
    u64 now = coop_now_ns();
    if (!prof_start) prof_start = now;
    if (++prof_frames < 600u) return;
    static const char *name[N_PROF] = { "host update", "guest update", "snapshot", "replay copies", "replay tick", "replay draw", "enemy AI" };
    double total = (double)(now - prof_start) / prof_frames / 1e6;
    printf("coop prof: %.3f ms per frame:", total);
    for (int i = 0; i < N_PROF; i++) printf(" %s %.3f", name[i], (double)prof_ns[i] / prof_frames / 1e6);
    printf("\n");
    memset(prof_ns, 0, sizeof prof_ns); prof_frames = 0; prof_start = now;
}

/* Once per field (psyq_etc.c). A mission is over when
 *   - the slot table moves (menus and briefings use their own), or
 *   - the player update has not run for 2 seconds and the host's player mode
 *     is no longer 1 (results, mission failed; a pause keeps mode 1), or
 *   - the host's player mode goes back to 2 (the next mission's intro).
 * The guest's aircraft goes away with it, and a guest that was destroyed may
 * fly again in the next mission. */
void ac3_coop_field(void) {
    if (!ac3_coop_guest) return;
    u32 mode_ptr = MEM_R32(PLAYER_MODE_PTR);
    u32 mode = ram_ptr(mode_ptr) ? MEM_R32(mode_ptr + MODE_STATE) : 0;
    int left = MEM_R32(SLOT_TABLE) != G.flight_table ||
               (ac3_vblank_count() - G.last_update_field > 120u && mode != 1u);
    int restart = mode == 2u && G.last_mode != 2u;
    G.last_mode = mode;
    if (!(G.live || G.dead) || !(left || restart)) return;
    if (G.live && ram_ptr(G.flight_table)) {
        u32 mine = slot_entity(G.flight_table, 1);
        if (ram_ptr(mine) && MEM_R32(mine + ENT_WORD) == G.handle) MEM_W32(mine + ENT_WORD, G.handle & ~ENT_LIVE);   /* free slot 1 */
    }
    printf("coop: mission over at field %u (%s); guest aircraft %s\n", ac3_vblank_count(),
           left ? "left flight" : "next mission starting", G.live ? "removed" : "may fly again");
    G.live = 0;
    G.dead = 0;
}

void ac3_coop_status(char *out, int cap) {
    snprintf(out, (size_t)cap, "guest %s handle 0x%05X created %u updates %u views %u deaths %u",
             G.live ? "live" : G.dead ? "destroyed" : "none", G.handle, G.created_field, G.updates, G.views, G.deaths);
}
