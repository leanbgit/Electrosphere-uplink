/*
 * coop_probe.h -- research instruments for the co-op work (docs/coop/).
 * Only built into the headless native bench (AC3_COOP_PROBE); the web build
 * has none of this. Options (after --boot):
 *
 *   --coop-hash FILE              one line per field: RAM / scratchpad /
 *                                 VRAM / SPU-state hashes and the scene ids
 *   --coop-ents FROM-TO[:N] FILE  the entity slot table every N fields:
 *                                 slot, pointer and the slot's first 0x100 bytes
 *   --coop-ram F1,F2,... DIR      full 2 MB RAM images at these fields
 *   --coop-wlog ADDR LEN FROM-TO FILE
 *                                 every guest write into [ADDR, ADDR+LEN)
 *                                 during those fields: field, pc, caller, value
 *   --coop-rlog ADDR LEN FROM-TO FILE
 *                                 the same for guest reads (interpreter loads)
 *   --coop-calls FROM-TO FILE     every guest function entered during those
 *                                 fields, with its caller and a call count
 *   --coop-args F1,F2,.. FROM-TO FILE
 *                                 a0-a3 and sp at each call of these functions
 *   --coop-wfn FUNC FROM-TO FILE  every write (outside the stack) made while
 *                                 FUNC and its callees run
 *   --coop-rfn FUNC FROM-TO FILE  the same for reads
 *   --coop-guest                  second aircraft driven by port 2 (runtime/hle/coop.h)
 *   --coop-view                   with --coop-guest: draw the guest's view instead
 *   --coop-p2 FILE                second controller from another recording
 *                                 (port 2 of an .ac3i file replaces port 2)
 */
#ifndef COOP_PROBE_H
#define COOP_PROBE_H
#include "ac3_runtime.h"

#ifdef AC3_COOP_PROBE
int  ac3_coop_probe_arg(int argc, char **argv, int *j);   /* 1 = consumed */
void ac3_coop_probe_field(void);                            /* once per field */
void ac3_coop_probe_finish(void);
void ac3_coop_probe_pads(void);                             /* after the recording's pads */
void ac3_coop_write_hit(u32 addr, u32 value, int size);
void ac3_coop_call(u32 target);
void ac3_coop_call_args(ctx_t *ctx, u32 target);
int  ac3_coop_wfn_begin(u32 target);
void ac3_coop_wfn_end(void);
void ac3_coop_replay_hle(u32 target);
int  ac3_coop_replaying(void);
extern u32 ac3_coop_r_lo, ac3_coop_r_hi;
void ac3_coop_read_hit(u32 addr, int size);
#endif
#endif
