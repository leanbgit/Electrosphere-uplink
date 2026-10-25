#include "hle.h"
#include "gpu_backend.h"
#include "spu_backend.h"
#include "ac3_capture.h"
#include "ac3_oracle.h"

static u32 vsync_callback_fn;
static u32 vblank_count;
static u32 vsync_ref;
static u32 intr_mask;
static int callbacks_stopped;

#define HBLANKS_PER_FRAME 263

u32 ac3_vblank_count(void) { return vblank_count; }
u32 ac3_hblank_count(void) { return vblank_count * HBLANKS_PER_FRAME; }

extern void ac3_pad_update(ctx_t *ctx);

extern void ac3_boot_frame_hook(void);
extern void ac3_cd_tick(ctx_t *ctx);
extern int  ac3_cd_debt_frames(void);

void ac3_frame_advance(ctx_t *ctx) {
    ac3_oracle_vblank(ctx);
    ac3_capture_frame(ctx);
    if (gpu->frame_end) gpu->frame_end();
    vblank_count++;
    ac3_oracle_pads();
    ac3_pad_update(ctx);
    ac3_cd_tick(ctx);
    ac3_spu_pump();
    if (vsync_callback_fn && !callbacks_stopped)
        ac3_call_indirect(ctx, vsync_callback_fn);
    if (gpu->frame_begin) gpu->frame_begin();
    ac3_boot_frame_hook();
}

void psyq_VSync(ctx_t *ctx) {
    if (ac3_hle_trace_enabled) {
        u32 gp  = ctx->r[28];
        u32 g   = MEM_R32(gp + 748);
        u32 req = MEM_R32(gp + 56), ring = MEM_R32(gp + 64);
        static u32 last_state = 0xFFFFFFFFu;
        u32 st = req ? MEM_R32(req + 32) : 0;

        {
            u32 gc = MEM_R32(gp + 1236);
            static u32 last_bytes = 0xFFFFFFFFu;
            if (gc) {
                u32 bs = MEM_R8U(gc) | (MEM_R8U(gc + 1) << 8) |
                         (MEM_R8U(gc + 2) << 16) | (MEM_R8U(gc + 3) << 24);
                u32 key = bs ^ (MEM_R8U(gc + 4) << 8) ^ (MEM_R8U(gc + 5) << 16);
                if (key != last_bytes) {
                    last_bytes = key;
                    hle_warn_always("gmv", "b[0..5] = %u %u %u %u %u %u at frame %u",
                                    MEM_R8U(gc), MEM_R8U(gc+1), MEM_R8U(gc+2),
                                    MEM_R8U(gc+3), MEM_R8U(gc+4), MEM_R8U(gc+5),
                                    vblank_count);
                }
            }
        }
        if (st != last_state) {
            last_state = st;
            hle_warn_always("stream", "state -> %-3u ring %-3d at frame %u",
                            st, ring ? (int)MEM_R32(ring + 52) : -1,
                            vblank_count);
        }
        hle_trace("VSync", "ra=0x%08X scene grp=%u sub=%u cur=%u req=%u "
                  "| stream state=%u ring=%d",
                  ctx->r[31],
                  g ? MEM_R32(g + 44) : 0, g ? MEM_R32(g + 48) : 0,
                  g ? MEM_R32(g + 52) : 0, g ? MEM_R32(g + 56) : 0,
                  st, ring ? (int)MEM_R32(ring + 52) : -1);
    }
    s32 mode = (s32)A0;
    if (mode < 0) { RET(vblank_count); return; }

    if (mode > 1) {
        for (int guard = 0;
             (s32)(vblank_count - vsync_ref) < mode && guard < 600; guard++)
            ac3_frame_advance(ctx);
    } else if (mode == 0) {
        ac3_frame_advance(ctx);
    }

    for (int guard = 0; guard < 600 && ac3_cd_debt_frames() > 0; guard++)
        ac3_frame_advance(ctx);

    u32 elapsed = (vblank_count - vsync_ref) * HBLANKS_PER_FRAME;
    if (mode != 1) vsync_ref = vblank_count;
    RET(elapsed);
}

void psyq_VSyncCallback(ctx_t *ctx) {
    u32 prev = vsync_callback_fn;
    vsync_callback_fn = A0;
    hle_trace("VSyncCallback", "0x%08X", A0);
    RET(prev);
}

void psyq_VSyncCallbacks(ctx_t *ctx) {
    vsync_callback_fn = A1;
    RET(0);
}

void psyq_ResetCallback(ctx_t *ctx) {
    vsync_callback_fn = 0;
    callbacks_stopped = 0;
    RET(0);
}

void psyq_StopCallback(ctx_t *ctx)    { callbacks_stopped = 1; RET(0); }
void psyq_RestartCallback(ctx_t *ctx) { callbacks_stopped = 0; RET(0); }
void psyq_CheckCallback(ctx_t *ctx)   { RET(callbacks_stopped ? 0 : 1); }

static u32 irq_handler[16];
static u32 dma_handler[8];

void psyq_InterruptCallback(ctx_t *ctx) {
    u32 irq = A0 & 15u, prev = irq_handler[irq];
    irq_handler[irq] = A1;
    hle_trace("InterruptCallback", "irq=%u fn=0x%08X", irq, A1);
    RET(prev);
}

void psyq_DMACallback(ctx_t *ctx) {
    u32 ch = A0 & 7u, prev = dma_handler[ch];
    dma_handler[ch] = A1;
    RET(prev);
}

void psyq_SetIntrMask(ctx_t *ctx) { u32 p = intr_mask; intr_mask = A0; RET(p); }
void psyq_GetIntrMask(ctx_t *ctx) { RET(intr_mask); }

void psyq_GetVideoMode(ctx_t *ctx) { RET(0); }

void psyq_startIntrVSync(ctx_t *ctx) { RET(0); }
void psyq_startIntrDMA(ctx_t *ctx)   { RET(0); }

void ac3_etc_machine_save(FILE *f) {
    ac3_ms_tag(f, "ETC");
    ac3_ms_u32(f, vsync_callback_fn);
    ac3_ms_u32(f, vblank_count);
    ac3_ms_u32(f, vsync_ref);
    ac3_ms_u32(f, intr_mask);
    ac3_ms_u32(f, (u32)callbacks_stopped);
}
