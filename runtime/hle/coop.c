/* coop.c -- see coop.h. Addresses are for the Rev 1 ACE3.BIN; the reasoning
 * behind each one is in docs/coop/memory_map.md. */
#include "coop.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int ac3_coop_guest;
static int coop_debug = -1;

extern void ac3_interpret(ctx_t *ctx, u32 entry);
extern u32 ac3_vblank_count(void);

#define GAME_GP        0x800BCB74u
#define SLOT_TABLE     (GAME_GP + 0x2E8u)  /* -> 64 entity pointers */
#define PLAYER_UPDATE  0x800729DCu         /* first call of the flight tick */
#define INPUT_MAP_FN   0x80028C24u         /* (controller, map, map+0x3C): applies the button config */
#define CTRL_PORT2     0x800BF388u         /* the game's decoded record for controller port 2 */
#define SLOT_SIZE      0x58u               /* slots 0 and 1 */
#define LIVE           0x10000u            /* handle bit: slot in use */

/* Per-player memory: the guest has its own copy, exchanged with the host's
 * around the guest's update. */
static const struct { u32 addr, len; } per_player[] = {
    { 0x801771E0u, 0xE0u },   /* player aircraft state (behind gp+0x514..0x524) */
    { 0x800BF3F0u, 0x40u },   /* input map (incl. the 4 bytes at map+0x3C) */
    { 0x800C7BE8u, 0x10u },   /* written by 0x80072114 every frame (position copy) */
};
#define N_PER_PLAYER (sizeof per_player / sizeof per_player[0])

/* Host-only memory the player update also writes (engine sound channels, one
 * library word): the guest's writes are discarded. */
static const struct { u32 addr, len; } host_only[] = {
    { 0x800C5830u, 0x40u },
    { 0x800BD0C0u, 0x04u },
};
#define N_HOST_ONLY (sizeof host_only / sizeof host_only[0])

static struct {
    int live;              /* guest aircraft exists in this flight */
    u32 handle;            /* its handle (slot 1) */
    u32 host_handle;       /* the host's handle when it was created */
    u32 created_field, updates;
    u8  mine[N_PER_PLAYER][0x100];   /* the guest's copy of each per-player range */
    u8  keep[N_HOST_ONLY][0x40];
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
    for (unsigned i = 0; i < N_PER_PLAYER; i++) copy_out(per_player[i].addr, G.mine[i], per_player[i].len);
    G.live = 1;
    G.host_handle = h;
    G.created_field = ac3_vblank_count();
    G.updates = 0;
    printf("coop: guest aircraft created in slot 1 (handle 0x%05X) at field %u\n", G.handle, G.created_field);
    return 1;
}

static void call_guest(ctx_t *ctx, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    ctx->r[4] = a0; ctx->r[5] = a1; ctx->r[6] = a2; ctx->r[7] = a3;
    ac3_interpret(ctx, fn);
}

static void guest_update(ctx_t *ctx, const ctx_t *entry) {
    if (coop_debug < 0) { const char *e = getenv("AC3_COOP_DEBUG"); coop_debug = e && *e && *e != '0'; }
    u32 slots = MEM_R32(SLOT_TABLE);
    if (!ram_ptr(slots)) return;
    u32 host = MEM_R32(slots), mine = MEM_R32(slots + 4);
    if (!ram_ptr(host) || !ram_ptr(mine)) return;
    u32 hh = MEM_R32(host);
    /* a new flight (the host's handle changed) or slot 1 reused: start over */
    if (G.live && (hh != G.host_handle || MEM_R32(mine) != G.handle)) {
        if ((MEM_R32(mine) & ~0x3Fu) != (G.handle & ~0x3Fu) || hh != G.host_handle)
            printf("coop: guest aircraft gone at field %u (slot 1 now 0x%05X)\n", ac3_vblank_count(), MEM_R32(mine));
        G.live = 0;
    }
    if (!G.live && !create_guest(slots)) return;

    /* swap in the guest */
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_out(host_only[i].addr, G.keep[i], host_only[i].len);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(per_player[i].addr, G.mine[i], per_player[i].len);
    MEM_W32(slots, mine);
    MEM_W32(slots + 4, host);

    ctx_t saved = *ctx;
    call_guest(ctx, INPUT_MAP_FN, CTRL_PORT2, 0x800BF3F0u, 0x800BF42Cu, 0);
    if (coop_debug && G.updates % 30u == 0) {
        printf("coop: field %u guest map", ac3_vblank_count());
        for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(0x800BF3F0u + i));
        printf("\n");
    }
    if (!getenv("AC3_COOP_NOUPDATE"))       /* research switch: create the guest but never update it */
        call_guest(ctx, PLAYER_UPDATE, entry->r[4], entry->r[5], entry->r[6], entry->r[7]);
    if (coop_debug && G.updates % 30u == 0) {
        u32 e = MEM_R32(slots);   /* still the guest's entity here */
        printf("coop: field %u guest missiles %u (host %u)\n", ac3_vblank_count(),
               MEM_R16U(0x801771E0u + 0xB4u), (u32)(G.mine[0][0xB4] | G.mine[0][0xB5] << 8));
        printf("coop: field %u guest block", ac3_vblank_count());
        for (u32 i = 0; i < 0x40u; i += 4) printf(" %08X", MEM_R32(0x801771E0u + i));
        printf("\n");
        printf("coop: field %u guest entity", ac3_vblank_count());
        for (u32 i = 0; i < 0x58u; i += 4) printf(" %08X", MEM_R32(e + i));
        printf("\n");
    }
    *ctx = saved;             /* the host's results (v0/v1) stand */

    /* swap the host back */
    MEM_W32(slots, host);
    MEM_W32(slots + 4, mine);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) exchange(per_player[i].addr, G.mine[i], per_player[i].len);
    for (unsigned i = 0; i < N_HOST_ONLY; i++) copy_in(host_only[i].addr, G.keep[i], host_only[i].len);
    G.updates++;
}

int ac3_coop_intercept(ctx_t *ctx, u32 target) {
    if (target != PLAYER_UPDATE) return 0;
    ctx_t entry = *ctx;
    ac3_interpret(ctx, target);          /* the host, as always */
    ctx_t host_result = *ctx;
    guest_update(ctx, &entry);
    *ctx = host_result;
    return 1;
}

void ac3_coop_status(char *out, int cap) {
    snprintf(out, (size_t)cap, "guest %s handle 0x%05X created %u updates %u",
             G.live ? "live" : "none", G.handle, G.created_field, G.updates);
}
