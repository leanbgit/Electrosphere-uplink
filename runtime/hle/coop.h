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
int  ac3_coop_intercept(ctx_t *ctx, u32 target);   /* 1 = handled the call */
void ac3_coop_status(char *out, int cap);   /* one-line state, for logs */
int  ac3_coop_replaying(void);              /* 1 while the guest's view is being replayed */
int  ac3_coop_gpu_ready(void);              /* 1 = a guest picture is waiting for DrawOTag */
int  ac3_coop_gpu_draw(int begin);          /* around DrawOTag's second pass: 1 = guest picture laid over RAM */
u16 *ac3_coop_display_pass(const u16 *game_vram);   /* the display VRAM for that pass */
u16 *ac3_coop_display_vram(u16 *game_vram);         /* what to show this field */
#endif
