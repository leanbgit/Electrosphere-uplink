#ifndef AC3_XA_ADPCM_H
#define AC3_XA_ADPCM_H

#include "hle.h"

int  ac3_xa_sector(const u8 *sector);

void ac3_xa_reset(void);
void ac3_xa_set_filter(int active, u8 file, u8 chan);
void ac3_xa_set_enabled(int on);
int  ac3_xa_get_enabled(void);
void ac3_xa_set_muted(int on);
void ac3_xa_stop(void);

void ac3_xa_stats_dump(void);
u32  ac3_xa_sectors_played(void);
u32  ac3_xa_peak(void);

#endif
