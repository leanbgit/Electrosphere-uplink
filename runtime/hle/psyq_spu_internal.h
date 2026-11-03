/*
 * psyq_spu_internal.h -- shared by the sound library files:
 *   psyq_spu.c         set-up, sound memory transfers, the mix each field,
 *                      noise, pitch LFO and interrupt settings
 *   psyq_spu_voice.c   voices: their settings, keys on and off, envelopes;
 *                      the master and CD volumes
 *   psyq_spu_reverb.c  sound memory allocation (SpuMalloc) and the reverb
 * The ear (spu_ear_psyq.c) compiles the three again as a second, private
 * chip: there every outside name gets "ear_" (spu_ear_names.h), and the names
 * below are made distinct the same way.
 */
#ifndef AC3_PSYQ_SPU_INTERNAL_H
#define AC3_PSYQ_SPU_INTERNAL_H
#include "spu_backend.h"
#include "spu_core.h"
#include <stdio.h>

#ifdef AC3_SPU_EAR
#define SPU_LIB(n) ear_spu_lib_##n
#else
#define SPU_LIB(n) spu_lib_##n
#endif

/* volume and envelope modes (libspu SPU_VOICE_*) */
#define SPU_VOICE_DIRECT      0
#define SPU_VOICE_LINEARIncN  1
#define SPU_VOICE_LINEARIncR  2
#define SPU_VOICE_LINEARDecN  3
#define SPU_VOICE_LINEARDecR  4
#define SPU_VOICE_EXPIncN     5
#define SPU_VOICE_EXPIncR     6
#define SPU_VOICE_EXPDec      7

#define apply_voice_switch   SPU_LIB(apply_voice_switch)
#define voice_machine_save   SPU_LIB(voice_machine_save)
#define rev_lib_reset        SPU_LIB(rev_lib_reset)
#define rev_area_allocated   SPU_LIB(rev_area_allocated)
#define reverb_machine_save  SPU_LIB(reverb_machine_save)

/* psyq_spu_voice.c: a voice bit set changed by SPU_ON / SPU_OFF / SPU_BIT */
u32  apply_voice_switch(u32 cur, long on_off, u32 bits);
void voice_machine_save(FILE *f);       /* master and CD volumes */
/* psyq_spu_reverb.c */
void rev_lib_reset(void);               /* reverb off, its work area released */
int  rev_area_allocated(u32 start_units);
void reverb_machine_save(FILE *f);      /* allocation and reverb settings */

#endif
