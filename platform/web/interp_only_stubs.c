/* Interpreter-only build: stand-ins for debug-oracle pieces that call
 * recompiled game functions by name (runtime/hle/ac3_oracle_frame.c is left
 * out of that build). They are reached only when oracle frame-stepping is
 * configured, which the web build never does. */
#ifdef AC3_INTERP_ONLY
#include "ac3_oracle.h"
int ac3_oracle_phase_stepped(ctx_t *ctx, u32 target) { (void)ctx; (void)target; return 0; }
#endif
