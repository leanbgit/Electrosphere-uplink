#ifndef AC3_STREAM_PROBE_H
#define AC3_STREAM_PROBE_H

#include "ac3_runtime.h"

void ac3_probe_stream_begin(ctx_t *ctx, u32 com, u32 lba,
                            u8 filter_file, u8 filter_chan);

void ac3_probe_stream_end(ctx_t *ctx, u32 lba);

void ac3_probe_sector(u32 lba, u8 file, u8 chan, u8 submode, int delivered);

void ac3_probe_read(u32 bytes, const u8 *data, u8 chan, u32 off);

void ac3_probe_mdec_in(u32 words, int bpp24);

void ac3_probe_script(ctx_t *ctx, u32 target);

void ac3_probe_queue(ctx_t *ctx);

void ac3_probe_unknown_insn(u32 pc, u32 word);

void ac3_probe_write_hit(u32 addr, u32 value, int size);

void ac3_probe_overlay_call(ctx_t *ctx, u32 target);

void ac3_probe_interp_pc(ctx_t *ctx, u32 pc);

void ac3_probe_load_image(int x, int y, int w, int h, u32 src);

void ac3_probe_unhandled_packet(u32 addr, u8 code, int len);

#endif
