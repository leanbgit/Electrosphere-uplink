/*
 * coop_internal.h -- shared by the co-op files (not part of the public API in
 * coop.h). Addresses are for the Rev 1 ACE3.BIN; the reasoning behind each one
 * is in docs/coop/memory_map.md and docs/coop/S2_guest.md; all of them have
 * names in coop_game.h.
 *
 * The co-op code is split by topic:
 *   coop.c          public entry points; routes each intercepted game call
 *   coop_guest.c    the guest's aircraft: creation, the identity swap around
 *                   the player update, its per-player memory
 *   coop_view.c     the guest's own picture (replay of the drawing part)
 *   coop_hud.c      HUD per player: lock-on state, warnings, missiles coming,
 *                   the other player on radar and screen
 *   coop_enemies.c  enemies treating the guest as a player: activation, the
 *                   aircraft AI, ground and ship weapons, missile launches
 *   coop_mission.c  mission rules, hazards and effects around the player
 *
 * The one idea used everywhere: the game believes "the player" is slot 0 of
 * the slot table and the memory around it. To make something happen for the
 * guest, the guest is put in slot 0 (and its memory swapped in) while the
 * game's own code runs, and everything is put back afterwards.
 */
#ifndef AC3_COOP_INTERNAL_H
#define AC3_COOP_INTERNAL_H
#include "coop.h"
#include "coop_game.h"         /* the game's addresses and data layouts */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void ac3_interpret(ctx_t *ctx, u32 entry);
extern void ac3_interpret_until(ctx_t *ctx, u32 pc, u32 stop);
extern u32  ac3_vblank_count(void);
extern u32  ac3_guest_ra;             /* return address of the call being intercepted */
extern u8  *ac3_scratch;

/* ---- the guest's copies of player memory (tables in coop_guest.c) --------- */
typedef struct { u32 addr, len; } coop_range_t;

/* swapped with the host's around the guest's update */
enum { PP_BLOCK, PP_INPUT_MAP, PP_POS_COPY, PP_TARGETING, PP_ALERT, N_PER_PLAYER };
extern const coop_range_t coop_per_player[N_PER_PLAYER];
/* written by the guest's update but kept as the host's */
enum { N_HOST_ONLY = 6 };
extern const struct coop_host_only { u32 addr, len; int world; } coop_host_only[N_HOST_ONLY];
/* the camera: world-structure ranges the guest's view keeps its own copy of */
enum { N_VIEW_PRIVATE = 2 };
extern const coop_range_t coop_view_private[N_VIEW_PRIVATE];
/* HUD memory kept per player from one frame to the next */
enum { HUD_WARN = 5, N_HUD_STATIC = 7 };
#define HUD_LOCK_LEN 8u
extern const coop_range_t coop_hud_static[N_HUD_STATIC];

/* ---- state ---------------------------------------------------------------- */
typedef struct {
    int dead;              /* destroyed this mission: spectating until the next one */
    u32 deaths, last_mode, flight_table, last_update_field;
    int live;              /* guest aircraft exists in this flight */
    u32 handle;            /* its handle (slot 1) */
    u32 created_field, updates;
    u8  mine[N_PER_PLAYER][0x100];   /* the guest's copy of each per-player range */
    u8  keep[N_HOST_ONLY][0x40];
    /* the guest's view */
    int view_on, cam_valid;
    u8  cam[N_VIEW_PRIVATE][0x100];
    /* the other player's HUD state (the guest's, except during the guest's
     * view replay, when it holds the host's) */
    int hud_valid;
    u8  hud_lock[HUD_LOCK_LEN];
    u8  hud_rec[N_SLOTS][HREC_SIZE];
    u8  hud_static[N_HUD_STATIC][0x480];
    u8  msg[MSG_STATE_LEN];  /* the other player's message on screen (BINGO, ...) */
    int quiet;             /* showing a message again on the other copy: no sound */
    /* the screen shake: what it was before the player updates, and the guest's */
    u8  shake_base[8], shake[8];
    int shake_valid;
    /* the guest's weapon-fire flash, and its value at the snapshot */
    u8  flash[FX_FLASH_LEN], flash_snap[FX_FLASH_LEN];
    int in_guest_update;   /* the guest's player update is running */
    u8 *snap_ram, *keep_buf;
    u8  snap_scratch[1024];
    u8  host_block[BLOCK_SIZE];   /* the host's aircraft block at the snapshot (the replay overwrites it) */
    ctx_t snap_ctx;
    u32 snap_pc;
    int replaying;         /* the guest's view replay is running */
    u32 views;
    u32 ai_guest_group;    /* enemy AI: bit n = aircraft in slot n is the guest's this frame */
} coop_state_t;
extern coop_state_t coop_g;
#define G coop_g
extern int coop_debug;     /* AC3_COOP_DEBUG: extra log lines */

/* ---- helpers -------------------------------------------------------------- */
static inline int ram_ptr(u32 p) { return (p >> 24) == 0x80u && (p & 0x1FFFFFu) < AC3_RAM_SIZE - 0x100u; }
static inline void copy_out(u32 addr, u8 *dst, u32 n) { for (u32 i = 0; i < n; i++) dst[i] = (u8)MEM_R8U(addr + i); }
static inline void copy_in(u32 addr, const u8 *src, u32 n) { for (u32 i = 0; i < n; i++) MEM_W8(addr + i, src[i]); }
static inline void exchange(u32 addr, u8 *other, u32 n) {
    for (u32 i = 0; i < n; i++) { u8 t = (u8)MEM_R8U(addr + i); MEM_W8(addr + i, other[i]); other[i] = t; }
}
/* call a game function with up to four arguments */
static inline void call_guest(ctx_t *ctx, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    ctx->r[4] = a0; ctx->r[5] = a1; ctx->r[6] = a2; ctx->r[7] = a3;
    ac3_interpret(ctx, fn);
}
/* where a per-player range is (the aircraft block moves between missions) */
static inline u32 pp_addr(unsigned i) { return i == PP_BLOCK ? MEM_R32(BLOCK_PTR) : coop_per_player[i].addr; }
/* the entity in a slot */
static inline u32 slot_entity(u32 slots, u32 n) { return MEM_R32(slots + 4u * n); }
/* |dx| + |dy| + |dz| between two entities, in world units */
static inline s32 dist_to(u32 e, u32 o) {
    s32 d = 0;
    for (int c = 0; c < 3; c++) { s32 v = (ent_coord(e, c) >> 12) - (ent_coord(o, c) >> 12); d += v < 0 ? -v : v; }
    return d;
}
static inline u32 flip01(u32 v) { return v == 0u ? 1u : v == 1u ? 0u : v; }

/* ---- timing (AC3_COOP_PROF=1: a line every 600 frames) ------------------- */
enum { PROF_HOST_UPDATE, PROF_GUEST_UPDATE, PROF_SNAPSHOT, PROF_REPLAY_COPY, PROF_REPLAY_TICK, PROF_REPLAY_DRAW, PROF_AI, N_PROF };
u64  coop_now_ns(void);
void coop_prof_add(int what, u64 since_ns);
void coop_prof_frame(void);

/* ---- between the files ---------------------------------------------------- */
/* coop_guest.c */
int  coop_player_update(ctx_t *ctx);            /* PLAYER_UPDATE: the host's, then the guest's */
/* coop_view.c */
void coop_view_mark(ctx_t *ctx, u32 resume_pc);
int  coop_view_after_drawing(ctx_t *ctx);       /* the frame's last drawing call */
int  coop_view_skip_3d(u32 target);              /* 1: skip this 3D drawing call (host's replay) */
/* coop_hud.c */
void coop_hud_swap(void);
void coop_threat_swap(void);
int  coop_hud_hook(ctx_t *ctx, u32 target);
int  coop_kill_messages(ctx_t *ctx, u32 target);  /* FN_AI_FIRST, FN_GROUND_DOWN */
void coop_message_log(void);
/* coop_enemies.c */
int  coop_enemies_hook(ctx_t *ctx, u32 target);
/* coop_mission.c */
void coop_guest_hazards(ctx_t *ctx, u32 e);
int  coop_mission_hook(ctx_t *ctx, u32 target);
void coop_flash_swap(void);                     /* the guest's weapon-fire flash in/out */
#endif
