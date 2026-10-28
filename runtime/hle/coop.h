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
int  ac3_coop_intercept(ctx_t *ctx, u32 target);   /* 1 = handled the call */
void ac3_coop_status(char *out, int cap);   /* one-line state, for logs */
#endif
