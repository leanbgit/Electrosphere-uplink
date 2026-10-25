#ifndef AC3_ORACLE_H
#define AC3_ORACLE_H

#include "hle.h"
#include "gpu_backend.h"

typedef struct {
    const char *record_input;
    const char *play_input;
    const char *out_dir;
    int blocks;
    u32 ram_every;
    int real_cards;
    const char *state_ticks;
    int rand_callers;
    u32 machine_tick;
    const char *frames_fields;
    u32 machine_field;
    int machine_phases;
} ac3_oracle_cfg_t;

int  ac3_oracle_configure(const ac3_oracle_cfg_t *cfg);
void ac3_oracle_finish(void);

int  ac3_oracle_in_draw(void);
void ac3_oracle_rand_caller(u32 ra);

int  ac3_oracle_deterministic(void);

void ac3_oracle_vblank(ctx_t *ctx);
void ac3_oracle_pads(void);

int  ac3_oracle_is_tick(u32 target);
void ac3_oracle_tick(ctx_t *ctx, u32 target, int after);
int  ac3_oracle_steps_frame(u32 target);
int  ac3_oracle_phase_stepped(ctx_t *ctx, u32 target);
void ac3_oracle_frame_snap(ctx_t *ctx, u32 target, u32 checkpoint);
int  ac3_oracle_phase_call(u32 ra);
void ac3_oracle_phase_snap(ctx_t *ctx, u32 target, u32 checkpoint);
void ac3_oracle_run_tick(ctx_t *ctx, u32 target, ac3_func_t f);

void ac3_oracle_cd(u32 lba, u32 dst, u32 bytes);

enum { AC3_OVRAM_UPLOAD, AC3_OVRAM_UPLOAD_OT, AC3_OVRAM_COPY, AC3_OVRAM_STORE, AC3_OVRAM_FILL };
void ac3_oracle_vram(int op, const PSX_RECT *r, u32 src, s32 dx, s32 dy);

void ac3_ms_u32(FILE *f, u32 v);
void ac3_ms_bytes(FILE *f, const void *p, u32 n);
void ac3_ms_tag(FILE *f, const char *tag);
void ac3_etc_machine_save(FILE *f);
void ac3_api_machine_save(FILE *f);
void ac3_spu_machine_save(FILE *f);
void ac3_spu_core_machine_save(FILE *f);
void ac3_xa_machine_save(FILE *f);
void ac3_cd_machine_save(FILE *f);
void ac3_pad_machine_save(FILE *f);
void ac3_pad2_machine_save(FILE *f);
void ac3_gpu_machine_save(FILE *f);
void ac3_gpu_sw_machine_save(FILE *f);

void ac3_cd_describe_lba(u32 lba, char *out, size_t n);

#endif
