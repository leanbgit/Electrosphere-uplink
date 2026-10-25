#ifndef AC3_SPU_CORE_H
#define AC3_SPU_CORE_H

#include "spu_backend.h"

#define SPU_SAMPLE_RATE 44100

#define SPU_FIELD_NUM  (SPU_SAMPLE_RATE * 1001)
#define SPU_FIELD_DEN  60000

int ac3_spu_field_frames(void);
#define SPU_FIELD_NOMINAL ((SPU_FIELD_NUM + SPU_FIELD_DEN - 1) / SPU_FIELD_DEN)

#define SPU_ST_OFF          0
#define SPU_ST_ON           1
#define SPU_ST_OFF_ENV_ON   2
#define SPU_ST_ON_ENV_OFF   3

#define SPU_SUCCESS         0
#define SPU_ERROR         (-1)
#define SPU_CHECK         (-1)
#define SPU_DIAG          (-2)
#define SPU_INVALID_ARGS  (-3)
#define SPU_LIB_OFF         0
#define SPU_LIB_ON          1
#define SPU_LIB_CLEAR       2
#define SPU_LIB_CDONLY      5
#define SPU_LIB_VOICEONLY   6
#define SPU_ST_STOP_LIB     2

#define SPU_REV_MODE_OFF        0
#define SPU_REV_MODE_ROOM       1
#define SPU_REV_MODE_STUDIO_A   2
#define SPU_REV_MODE_STUDIO_B   3
#define SPU_REV_MODE_STUDIO_C   4
#define SPU_REV_MODE_HALL       5
#define SPU_REV_MODE_SPACE      6
#define SPU_REV_MODE_ECHO       7
#define SPU_REV_MODE_DELAY      8
#define SPU_REV_MODE_PIPE       9
#define SPU_REV_MODE_MAX       10
#define SPU_REV_MODE_CLEAR_WA  0x100

void ac3_spu_core_reset(void);

void ac3_spu_core_key_on(int v);
void ac3_spu_core_key_off(int v);
void ac3_spu_core_attr_changed(int v);
void ac3_spu_core_set_loop_addr(int v, u32 addr);
u32  ac3_spu_core_loop_addr(int v);

int  ac3_spu_core_key_status(int v);
s16  ac3_spu_core_envx(int v);
void ac3_spu_core_volx(int v, s16 *l, s16 *r);

void ac3_spu_set_master(s16 l, s16 r, s16 mode_l, s16 mode_r);
void ac3_spu_master_volx(s16 *l, s16 *r);
void ac3_spu_set_mute(int on);
int  ac3_spu_get_mute(void);

void ac3_spu_set_cd_volume(s16 l, s16 r);
void ac3_spu_set_cd_reverb(int on);
void ac3_spu_set_cd_mix(int on);
int  ac3_spu_get_cd_mix(void);
void ac3_spu_cd_push(const s16 *interleaved, int nframes);
int  ac3_spu_cd_queued(void);
void ac3_spu_cd_flush(void);

void ac3_spu_rvb_set_reg(int reg, u16 v);
u16  ac3_spu_rvb_reg(int reg);
void ac3_spu_rvb_set_base(u16 v);
u16  ac3_spu_rvb_base(void);
void ac3_spu_rvb_set_volume(s16 l, s16 r);
void ac3_spu_rvb_volume(s16 *l, s16 *r);
void ac3_spu_rvb_set_enable(int on);
int  ac3_spu_rvb_enabled(void);
void ac3_spu_rvb_run(const s32 in[2], s32 out[2]);
void ac3_spu_set_reverb_voices(u32 bits);
u32  ac3_spu_get_reverb_voices(void);

u32  ac3_spu_reverb_work_size(int mode);
u32  ac3_spu_reverb_work_start(int mode);
const u16 *ac3_spu_reverb_preset(int mode);

void ac3_spu_set_noise_voices(u32 bits);
u32  ac3_spu_get_noise_voices(void);
void ac3_spu_set_noise_clock(int clock);
int  ac3_spu_get_noise_clock(void);
void ac3_spu_set_pitch_lfo_voices(u32 bits);
u32  ac3_spu_get_pitch_lfo_voices(void);

u16 ac3_spu_note_to_pitch(u16 note, u16 sample_note);

void ac3_spu_core_mix(s16 *out, int nframes);

#define SPU_DECODED_SIZE 0x200
const s16 *ac3_spu_decoded(int which);

u32 ac3_spu_output_peak(void);
u64 ac3_spu_frames_mixed(void);
void ac3_spu_stats_dump(void);

int  ac3_spu_dump_open(const char *path);
void ac3_spu_dump_write(const s16 *frames, int nframes);
void ac3_spu_dump_close(void);

#endif
