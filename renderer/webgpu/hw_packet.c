/*
 * hw_packet.c -- the game-worker side of the enhanced renderer in the browser.
 *
 * renderer/hw/hw.c records each field's drawing into hw_state (commands,
 * vertices, staged uploads) exactly as in the native build. Natively,
 * hw_record() then turns that into Vulkan work on the same thread. Here the
 * GPU lives in a separate render worker (web/render-worker.js), because the
 * game thread never returns to its event loop and so can never learn when the
 * GPU has finished a frame. hw_packet_build() packs the field into one buffer
 * for that worker and clears the lists, as hw_record() would.
 */
#include "hw_packet.h"
#include "hw_fx_web.h"
#include <stdlib.h>
#include <string.h>

#define H hw_state   /* defined in renderer/hw/hw.c */

int hw_init_cpu(const hw_config_t *cfg) {
    memset(&H, 0, sizeof H);
    H.S = cfg && cfg->scale > 0 ? cfg->scale : 1;
    if (H.S > HW_MAX_SCALE) H.S = HW_MAX_SCALE;
    H.persp = cfg ? cfg->perspective : 1;
    H.W = (u32)(VRAM_W * H.S);
    H.Hs = (u32)(VRAM_H * H.S);
    H.ready = 1;
    hw_fx_init();
    return 1;
}

void hw_shutdown(void) {
    if (!H.ready) return;
    hw_fx_shutdown();
    free(H.verts); free(H.staging); free(H.cmds);
    memset(&H, 0, sizeof H);
}

static u8 *buf;
static size_t cap;

size_t hw_packet_build(u8 **out, u32 seq, u32 flags, const gpu_display_t *d, const u16 *vram) {
    size_t c = (size_t)H.ncmds * sizeof(cmd_t), v = (size_t)H.nverts * sizeof(hw_vtx_t);
    size_t s = PKT_ALIGN4(H.nstaging), r = (flags & PKT_VRAM) && vram ? (size_t)VRAM_W * VRAM_H * 2 : 0;
    if (!r) flags &= ~PKT_VRAM;
    size_t fx = PKT_ALIGN4(hw_fx_pack_size());      /* world effects: scenes, tiles, map */
    size_t n = sizeof(hw_packet_hdr_t) + c + v + s + fx + r;
    if (n > cap) {
        u8 *nb = (u8 *)realloc(buf, n);
        if (!nb) { *out = NULL; return 0; }
        buf = nb; cap = n;
    }
    hw_packet_hdr_t *h = (hw_packet_hdr_t *)buf;
    memset(h, 0, sizeof *h);
    h->magic = PKT_MAGIC; h->seq = seq; h->flags = flags;
    h->ncmds = H.ncmds; h->nverts = H.nverts; h->nstaging = (u32)s; h->fx_bytes = (u32)fx;
    if (d) h->d = *d;
    u8 *p = buf + sizeof *h;
    if (c) memcpy(p, H.cmds, c);       p += c;
    if (v) memcpy(p, H.verts, v);      p += v;
    if (H.nstaging) memcpy(p, H.staging, H.nstaging);
    p += s;
    if (fx) hw_fx_pack(p);
    p += fx;
    if (r) memcpy(p, vram, r);

    /* the field has been handed over: same bookkeeping as the end of hw_record() */
    H.nverts = H.nstaging = H.ncmds = 0;
    hw_fx_frame_done();
    H.last_prims = H.prims;
    H.last_precise = H.precise;
    H.prims = H.precise = 0;
    *out = buf;
    return n;
}
