/* coop_hangar.c -- the guest's turn in the hangar (docs/coop/S4_plane_choice.md).
 *
 * Before a mission the host picks a plane and weapons in the game's hangar
 * (an overlay, the same in the campaign and in simulation mode). In co-op the
 * hangar then runs a second time with controller 2 in charge, for the guest;
 * the guest's choice becomes its plane and weapons in the mission
 * (coop_guest.c, guest_loadout), and the host's choice is put back.
 *
 * The hangar is a set of screens run from a table by its state (HANGAR_PTR,
 * halfword +0): 0 enter the plane list, 1 plane list, 2..3 plane confirmed,
 * 4 weapon select, 5 leaving it (+2: 1 confirmed, 2 cancelled). Cancelling in
 * the plane list leaves the hangar from state 2 (+2 = 2). Leaving with a
 * confirmation commits the choice into the scene (FN_HANGAR_COMMIT), frees
 * the hangar and goes on to the mission.
 *
 *   host confirms   the commit runs (the host's choice is in the scene), and
 *                   is kept; freeing the hangar and going on are skipped; the
 *                   hangar goes back to state 0, and controller 2 drives it.
 *   guest confirms  the commit runs; the scene's choice is the guest's, which
 *                   is kept, and the host's is put back; the hangar goes on
 *                   to the mission as usual.
 *   guest cancels   in its weapon select: back to its plane list; in its
 *                   plane list: back to the host's plane list (the host
 *                   confirms again, then it is the guest's turn again).
 *
 * Both computers see every controller's input every field (lockstep), so
 * they take the same turns at the same fields. */
#include "coop_internal.h"

/* the hangar overlay: these addresses are reused by other overlays, so its
 * code is recognised by its first instructions before anything is done */
#define FN_HANGAR_PLANE_LEAVE 0x800FA4E4u   /* state 2's handler: leaving the plane list (+2: 1 confirmed, 2 cancelled) */
#define FN_HANGAR_LEAVE   0x800FAA48u   /* state 5's handler: leaving the weapon select */
#define FN_HANGAR_COMMIT  0x800F9B10u   /* the choice into the scene (SCENE_PLANE was set earlier, SCENE_WEAPONS here) */
#define RA_HANGAR_FREE    0x800FAAD0u   /*   after it, in FN_HANGAR_LEAVE: free the hangar (0x80027538) ... */
#define RA_HANGAR_NEXT    0x800FAADCu   /*   ... and go on to the next screen (0x800F7524(3, 0)) */
#define FN_HEAP_FREE_TO   0x80027538u   /* (p): the heap's top back to p */
#define FN_MENU_NEXT      0x800F7524u   /* (screen, 0): the next menu screen */
#define MENU_PTR          0x80112DC4u   /* -> the menus' state: */
#define MENU_REQUEST      0x84u         /*   halfwords: the screen to go to next (FN_HANGAR_LEAVE sets 3, 0) */
#define HANGAR_PTR        0x80112E10u   /* -> the hangar's state */
#define HANGAR_STATE      0x00u         /* halfword */
#define HANGAR_LEAVING    0x02u         /* halfword, in state 5: 1 confirmed, 2 cancelled */
#define HANGAR_ENTER      0u
#define HANGAR_CONFIRMED  1

extern int ac3_pad_swap;                /* psyq_pad.c: 1 = controller 2 is read as controller 1 */

static struct {
    int guest_turn;                     /* the guest is choosing */
    int skip_exit;                      /* the host just committed: don't leave the hangar */
    u8  host_plane[2], host_weapons[SCENE_WEAPONS_LEN];
} H;

int ac3_coop_hangar_turn_on = 1;        /* 0: no guest's turn (--coop-no-hangar-turn, recordings made without it) */

static int hangar_here(void) {
    return ac3_coop_hangar_turn_on && MEM_R32(FN_HANGAR_LEAVE) == 0x3C028011u && MEM_R32(FN_HANGAR_LEAVE + 4) == 0x8C422E10u &&
           MEM_R32(FN_HANGAR_COMMIT) == 0x3C02800Cu && MEM_R32(FN_HANGAR_COMMIT + 4) == 0x8C43CE60u;
}
static u32 hangar(void) { u32 h = MEM_R32(HANGAR_PTR); return ram_ptr(h) ? h : 0u; }

static void end_turn(void) {
    H.guest_turn = 0;
    ac3_pad_swap = 0;
}

static int commit(ctx_t *ctx) {
    ac3_interpret(ctx, FN_HANGAR_COMMIT);
    if (!H.guest_turn) {                                    /* the host's choice */
        copy_out(SCENE + SCENE_PLANE, H.host_plane, 2);
        copy_out(SCENE + SCENE_WEAPONS, H.host_weapons, SCENE_WEAPONS_LEN);
        H.skip_exit = 1;
        printf("coop: hangar: host's plane %u, weapons %u,%u,%u; the guest's turn\n", H.host_plane[0],
               H.host_weapons[0], H.host_weapons[1], H.host_weapons[2]);
        return 1;
    }
    ac3_coop_guest_plane = MEM_R16S(SCENE + SCENE_PLANE);   /* the guest's choice */
    for (u32 i = 0; i < SCENE_WEAPONS_LEN; i++) ac3_coop_guest_weapons[i] = (u8)MEM_R8U(SCENE + SCENE_WEAPONS + i);
    copy_in(SCENE + SCENE_PLANE, H.host_plane, 2);
    copy_in(SCENE + SCENE_WEAPONS, H.host_weapons, SCENE_WEAPONS_LEN);
    printf("coop: hangar: guest's plane %d, weapons %u,%u,%u\n", ac3_coop_guest_plane,
           ac3_coop_guest_weapons[0], ac3_coop_guest_weapons[1], ac3_coop_guest_weapons[2]);
    end_turn();
    return 1;
}

static int leave(ctx_t *ctx) {
    u32 h = hangar();
    if (!h) return 0;
    if (H.guest_turn && MEM_R16S(h + HANGAR_LEAVING) != HANGAR_CONFIRMED) {
        MEM_W16(h + HANGAR_STATE, HANGAR_ENTER);           /* the guest cancelled: its plane list again */
        MEM_W16(h + HANGAR_LEAVING, 0);
        return 1;
    }
    u32 menu = MEM_R32(MENU_PTR), request = ram_ptr(menu) ? MEM_R32(menu + MENU_REQUEST) : 0;
    ac3_interpret(ctx, FN_HANGAR_LEAVE);
    if (H.skip_exit) {                                      /* the host committed: the guest's turn */
        H.skip_exit = 0;
        if (ram_ptr(menu)) MEM_W32(menu + MENU_REQUEST, request);   /* no next screen yet */
        MEM_W16(h + HANGAR_STATE, HANGAR_ENTER);
        MEM_W16(h + HANGAR_LEAVING, 0);
        H.guest_turn = 1;
        ac3_pad_swap = 1;
    }
    return 1;
}

/* The guest cancelled in the plane list: back to the host, whose plane list
 * comes back with the host's choice in the scene (it confirms again, and the
 * guest gets another turn). */
static int plane_leave(void) {
    u32 h = hangar();
    if (!h || !H.guest_turn || MEM_R16S(h + HANGAR_LEAVING) == HANGAR_CONFIRMED) return 0;
    copy_in(SCENE + SCENE_PLANE, H.host_plane, 2);
    copy_in(SCENE + SCENE_WEAPONS, H.host_weapons, SCENE_WEAPONS_LEN);
    end_turn();
    MEM_W16(h + HANGAR_STATE, HANGAR_ENTER);
    MEM_W16(h + HANGAR_LEAVING, 0);
    printf("coop: hangar: the guest cancelled; the host's turn again\n");
    return 1;
}

int coop_hangar_hook(ctx_t *ctx, u32 target) {
    switch (target) {
    case FN_HANGAR_PLANE_LEAVE: return hangar_here() && plane_leave();
    case FN_HANGAR_LEAVE:  return hangar_here() && leave(ctx);
    case FN_HANGAR_COMMIT: return hangar_here() && commit(ctx);
    }
    if (H.skip_exit && ((target == FN_HEAP_FREE_TO && ac3_guest_ra == RA_HANGAR_FREE) ||
                        (target == FN_MENU_NEXT && ac3_guest_ra == RA_HANGAR_NEXT))) return 1;   /* skipped */
    return 0;
}

/* The flight began without the guest's turn finishing (it should not
 * happen): give controller 1 back. */
void coop_hangar_flight(void) {
    if (H.guest_turn || ac3_pad_swap) { printf("coop: hangar: the guest's turn did not finish\n"); end_turn(); }
    H.skip_exit = 0;
}

int ac3_coop_hangar_turn(void) { return H.guest_turn; }
