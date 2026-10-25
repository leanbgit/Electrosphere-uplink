#ifndef AC3_SPU_BACKEND_H
#define AC3_SPU_BACKEND_H

#include "hle.h"

#define SPU_RAM_SIZE   0x80000u
#define SPU_VOICES     24

typedef struct {
    u32 start_addr;
    u32 loop_addr;

    u16 pitch;
    u16 note, sample_note;

    s16 vol_left, vol_right;
    s16 volmode_left, volmode_right;

    u16 adsr1;
    u16 adsr2;

    int keyed_on;
} spu_voice_t;

#define SPU_ADSR_AR(a1)     (((a1) >> 8) & 0x7Fu)
#define SPU_ADSR_AMODE(a1)  (((a1) >> 15) & 1u)
#define SPU_ADSR_DR(a1)     (((a1) >> 4) & 0x0Fu)
#define SPU_ADSR_SL(a1)     ((a1) & 0x0Fu)
#define SPU_ADSR_SR(a2)     (((a2) >> 6) & 0x7Fu)
#define SPU_ADSR_SMODE(a2)  (((a2) >> 15) & 1u)
#define SPU_ADSR_SDIR(a2)   (((a2) >> 14) & 1u)
#define SPU_ADSR_RR(a2)     ((a2) & 0x1Fu)
#define SPU_ADSR_RMODE(a2)  (((a2) >> 5) & 1u)

#define SPU_ADSR1_PACK(am, ar, dr, sl) \
    (u16)((((am) & 1u) << 15) | (((ar) & 0x7Fu) << 8) | \
          (((dr) & 0x0Fu) << 4) | ((sl) & 0x0Fu))
#define SPU_ADSR2_PACK(sm, sd, sr, rm, rr) \
    (u16)((((sm) & 1u) << 15) | (((sd) & 1u) << 14) | (((sr) & 0x7Fu) << 6) | \
          (((rm) & 1u) << 5) | ((rr) & 0x1Fu))

typedef struct {
    const char *name;
    void (*init)(void);
    void (*shutdown)(void);

    void (*key_on)(int v, const spu_voice_t *voice);
    void (*key_off)(int v);

    void (*upload)(u32 addr, const u8 *data, u32 size);

    void (*set_master_volume)(s16 left, s16 right);
    void (*set_cd_volume)(s16 left, s16 right);

    int  (*frames_wanted)(void);
    void (*submit)(const s16 *frames, int nframes);
} spu_backend_t;

extern const spu_backend_t *spu;
extern const spu_backend_t spu_backend_null;
extern const spu_backend_t spu_backend_sdl;

extern u8 *ac3_spu_ram;
extern spu_voice_t ac3_spu_voice[SPU_VOICES];

void spu_select_backend(const spu_backend_t *b);

u8 ac3_spu_decode_block(const u8 *block, s16 *out28, s16 hist[2]);

void ac3_spu_pump(void);

#endif
