#ifndef AC3_PGXP_H
#define AC3_PGXP_H

#include "ac3_runtime.h"

typedef struct {
    float sx, sy;
    float vx, vy, vz;
} ac3_pgxp_vtx_t;

#ifdef AC3_ENHANCED
void ac3_pgxp_record(u32 key, const ac3_pgxp_vtx_t *v);
int  ac3_pgxp_lookup(u32 key, ac3_pgxp_vtx_t *out);
int  ac3_pgxp_lookup_all(u32 key, ac3_pgxp_vtx_t *out, u32 *gen, int max);
int  ac3_pgxp_edge_key(u32 key);
void ac3_pgxp_next_frame(void);

extern float ac3_ws_factor;

#define AC3_WS_FRUSTUM_CALLER 0x8005F950u
extern int   ac3_pgxp_enabled;

extern int   ac3_gte_no_depthcue;

extern u32 ac3_pgxp_projections;
extern void (*ac3_pgxp_tap)(u32 key, const ac3_pgxp_vtx_t *v);
#endif

#endif
