/*
 * hw_packet.h -- one field of enhanced-renderer work, as sent from the game
 * worker (which records it: renderer/hw/hw.c) to the render worker (which
 * draws it: renderer/webgpu/hw_webgpu.c). Both sides are built by the same
 * compiler for the same target, so the structs are copied as they are.
 *
 *   hw_packet_hdr_t
 *   cmd_t     [ncmds]
 *   hw_vtx_t  [nverts]
 *   u8        [nstaging]      (VRAM uploads, RGBA8 rows)
 *   u8        [fx_bytes]      (world effects: hw_fx_web.h)
 *   u16       [1024*512]      (raw VRAM, only when PKT_VRAM: movies / original picture)
 * Every part starts on a 4-byte boundary.
 */
#ifndef AC3_HW_PACKET_H
#define AC3_HW_PACKET_H
#include "hw_private.h"

#define PKT_MAGIC 0x50334341u   /* "AC3P" */
#define PKT_HD    1u            /* draw the HD picture (else the original from VRAM) */
#define PKT_WIDE  2u            /* 16:9 display area */
#define PKT_VRAM  4u            /* raw VRAM included */

typedef struct {
    u32 magic, seq, ncmds, nverts, nstaging, flags;
    gpu_display_t d;
    u32 fx_bytes;
} hw_packet_hdr_t;

#define PKT_ALIGN4(x) (((x) + 3u) & ~3u)

/* game side (renderer/webgpu/hw_packet.c) */
int    hw_init_cpu(const hw_config_t *cfg);
size_t hw_packet_build(u8 **out, u32 seq, u32 flags, const gpu_display_t *d, const u16 *vram);

#endif
