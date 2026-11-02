/* coop_sync.h -- fingerprints of the game's state, for checking that two
 * computers (or two builds, or two browsers) run the game identically.
 *
 * In every build (native, bench, web):
 *   --coop-sync-log FILE   one line per field: the field, the scene ids and
 *                          hashes of RAM (+ the extra memory), scratchpad,
 *                          video memory (+ the second bank), the sound
 *                          chip's state, and "sim": RAM without the picture
 *                          buffers (what the host's and the guest's computers
 *                          must agree on; the rest differs by whose view is
 *                          drawn). Same format as the bench's --coop-hash;
 *                          compare with tools/coop/hashcmp.py.
 *
 * Lockstep networking will use the same "sim" fingerprint to spot a desync.
 */
#ifndef COOP_SYNC_H
#define COOP_SYNC_H
#include "ac3_runtime.h"
#include <stdio.h>

int  ac3_coop_sync_log_open(const char *path);   /* 1 = opened */
void ac3_coop_sync_field(void);                  /* once per field (host/boot.c) */
void ac3_coop_sync_log_close(void);

void ac3_coop_sync_header(FILE *f);
void ac3_coop_sync_row(FILE *f, u32 field);      /* one line, as described above */
u64  ac3_coop_sync_sim(void);                    /* the "sim" fingerprint */
#endif
