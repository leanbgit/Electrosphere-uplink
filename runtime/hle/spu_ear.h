/* spu_ear.h -- a second sound chip for what one player hears (co-op,
 * docs/coop/S5_sound.md).
 *
 * The game's sound chip (spu_core.c, psyq_spu.c) must stay identical on both
 * computers: the game reads its state back every frame. So in co-op it keeps
 * running exactly as the game drives it, but what the speakers play can come
 * from a second chip instead, "the ear": the same code compiled again under
 * other names (spu_ear_core.c, spu_ear_psyq.c, spu_ear_names.h), with its own
 * voices, mixer and sound memory. Nothing the game reads comes from the ear.
 *
 * The real chip copies to the ear what does not depend on whose sound it is:
 * sample uploads into sound memory, CD audio (music, radio). coop_sound.c
 * drives the ear's voices.
 */
#ifndef SPU_EAR_H
#define SPU_EAR_H
#include "ac3_runtime.h"
#include "spu_backend.h"

/* The ear's sound memory is bigger than the PS1's: the first 512 KB are the
 * same as the real chip's; past them is room for sounds only the ear plays
 * (the guest's plane sounds, coop_sound.c). */
#define EAR_SAMPLE_SPACE 0x100000u
#define EAR_EXTRA        0x80000u

extern int ac3_spu_ear_on;      /* the ear exists: the real chip copies uploads and CD audio to it */
extern int ac3_spu_ear_heard;   /* the speakers play the ear, not the real chip */
extern int ac3_spu_ear_mirror;  /* the ear copies the real chip's voices before each mix */

/* the ear's side (spu_ear_core.c, spu_ear_psyq.c) */
extern u8 *ear_ac3_spu_ram;
extern spu_voice_t ear_ac3_spu_voice[SPU_VOICES];
void ear_ac3_spu_core_mix(s16 *out, int nframes);
void ear_ac3_spu_cd_push(const s16 *interleaved, int nframes);
void ear_ac3_spu_core_key_off(int v);
int  ear_ac3_spu_dump_open(const char *path);
void ear_ac3_spu_dump_write(const s16 *frames, int nframes);
void ear_ac3_spu_dump_close(void);
void ear_psyq_SpuSetVoiceAttr(ctx_t *ctx);
void ear_psyq_SpuSetKey(ctx_t *ctx);
void ear_psyq_SpuGetAllKeysStatus(ctx_t *ctx);
void ear_psyq_SpuSetCommonAttr(ctx_t *ctx);

/* the mixer's state, to copy one chip's into the other (spu_core.c) */
#define SPU_STATE_SETTINGS 1u   /* master and CD volume, reverb and noise settings */
#define SPU_STATE_VOICES   2u   /* every voice's playback and envelope */
#define SPU_STATE_RUNNING  4u   /* the reverb's and the mixer's running state */
#define SPU_STATE_CD       8u   /* the queue of CD audio */
#define SPU_STATE_ALL      15u
u32  ac3_spu_core_state_size(void);
void ac3_spu_core_state_save(u8 *to);
void ac3_spu_core_state_load(const u8 *from, u32 parts);
void ear_ac3_spu_core_state_load(const u8 *from, u32 parts);

/* the ear made identical to the real chip (voices, mixer, sound memory) */
void ac3_spu_ear_copy_all(void);
/* the ear's voices and mixer made identical to the real chip's (not its sound memory) */
void ac3_spu_ear_follow(u32 parts);
#endif
