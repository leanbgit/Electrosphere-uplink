/*
 * coop.h -- the co-op guest's aircraft (prototype, see docs/coop/S2_guest.md).
 *
 * When enabled, every call of the game's player update (0x800729DC) is
 * followed by a second call for the guest: the guest's copy of the player's
 * state is swapped in, controller port 2 drives it, and the host's state is
 * swapped back afterwards. Interpreter builds only (the website): the hook
 * sits where the interpreter enters guest functions.
 */
#ifndef AC3_COOP_H
#define AC3_COOP_H
#include "ac3_runtime.h"

extern int ac3_coop_guest;                 /* 1 = a second, guest-controlled aircraft */
extern int ac3_coop_view;                  /* 1 = draw the guest's view (the guest's computer) */
extern int ac3_coop_split;                 /* 1 = show both views (one computer, local test) */
extern u8  ac3_coop_guest_weapons[3];      /* the guest's hangar choice (SCENE_WEAPONS); 0 = as the host's */
extern int ac3_coop_guest_plane;           /* the guest's plane (SCENE_PLANE, 0..36); -1 = as the host's */
int  ac3_coop_hangar_turn(void);            /* 1 while the guest is choosing in the hangar (controller 2 drives it) */
extern int ac3_coop_ears;                  /* whose sound the speakers play: -1 by computer, 0 host's, 1 guest's */
extern int ac3_coop_hangar_turn_on;        /* 0 = no guest's turn in the hangar (for recordings made without one) */
int  ac3_coop_intercept(ctx_t *ctx, u32 target);   /* 1 = handled the call */
/* who has the controls now */
#define AC3_COOP_PHASE_OFF     (-1)   /* no co-op */
#define AC3_COOP_PHASE_HOST      0    /* menus, briefings, the host's hangar turn: the host only */
#define AC3_COOP_PHASE_HANGAR    1    /* the guest's turn in the hangar: the guest only */
#define AC3_COOP_PHASE_FLYING    2    /* both fly */
#define AC3_COOP_PHASE_WAITING   3    /* a mission, before the guest's plane joins (when the host gets control) */
#define AC3_COOP_PHASE_DOWN      4    /* the guest was shot down: watching until the mission ends */
int  ac3_coop_phase(void);
void ac3_coop_status(char *out, int cap);   /* one-line state, for logs */
int  ac3_coop_replaying(void);
void ac3_coop_field(void);                  /* once per field */              /* 1 while the guest's view is being replayed */
int  ac3_coop_gpu_ready(void);              /* 1 = a guest picture is waiting for DrawOTag */
int  ac3_coop_gpu_draw(int begin);          /* around DrawOTag's second pass: 1 = guest picture laid over RAM */
u16 *ac3_coop_display_pass(const u16 *game_vram);   /* the display VRAM for that pass */
u16 *ac3_coop_display_vram(u16 *game_vram);         /* what to show this field */
#endif
