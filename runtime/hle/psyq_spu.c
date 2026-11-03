/*
 * psyq_spu.c -- the game's sound library (libspu): set-up, transfers to and
 * from sound memory, and the mix of each field. Voices are in
 * psyq_spu_voice.c, memory allocation and reverb in psyq_spu_reverb.c; the
 * chip itself is emulated in spu_core.c.
 */
#include "coop.h"
#include "psyq_spu_internal.h"
#include "spu_ear.h"
#include "ac3_oracle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

u8 *ac3_spu_ram = NULL;
spu_voice_t ac3_spu_voice[SPU_VOICES];

/* transfers (SpuSetTransferMode) */
#define SPU_TRANSFER_BY_DMA  0
#define SPU_TRANSFER_PEEK    0
#define SPU_TRANSFER_WAIT    1

static u32 transfer_addr = 0x1010;
static u32 transfer_mode;
static int spu_ready;
static u32 transfer_callback_fn;
static u32 irq_callback_fn;
static u32 irq_addr;
static int irq_on;

static void null_init(void) { }
static void null_shutdown(void) { }
static void null_key_on(int v, const spu_voice_t *voice) { (void)v; (void)voice; }
static void null_key_off(int v) { (void)v; }
static void null_upload(u32 a, const u8 *d, u32 s) { (void)a; (void)d; (void)s; }
static void null_master(s16 l, s16 r) { (void)l; (void)r; }
static void null_cdvol(s16 l, s16 r) { (void)l; (void)r; }

static int null_frames_wanted(void) { return ac3_spu_field_frames(); }
static void null_submit(const s16 *f, int n) { (void)f; (void)n; }

const spu_backend_t spu_backend_null = {
    "null", null_init, null_shutdown,
    null_key_on, null_key_off, null_upload, null_master, null_cdvol,
    null_frames_wanted, null_submit,
};

const spu_backend_t *spu = &spu_backend_null;

void spu_select_backend(const spu_backend_t *b) {
    if (spu && spu->shutdown) spu->shutdown();
    spu = b;
    if (spu && spu->init) spu->init();
}

static void spu_ram_init(void) {
    if (!ac3_spu_ram) ac3_spu_ram = (u8 *)calloc(1, SPU_RAM_SIZE);
}

/* coop_sync.c: hash of the SPU state the game can observe (RAM + voices).
 * Field by field, so the structure's padding and layout (which differ
 * between compilers) don't count. */
void ac3_spu_state_hash(u64 *h) {
    u64 x = 0xCBF29CE484222325ull;
#define MIX(v) (x ^= (u64)(u32)(v), x *= 0x100000001B3ull)
    for (int i = 0; i < SPU_VOICES; i++) {
        const spu_voice_t *v = &ac3_spu_voice[i];
        MIX(v->start_addr); MIX(v->loop_addr); MIX(v->pitch); MIX(v->note); MIX(v->sample_note);
        MIX((u16)v->vol_left); MIX((u16)v->vol_right); MIX((u16)v->volmode_left); MIX((u16)v->volmode_right);
        MIX(v->adsr1); MIX(v->adsr2); MIX(v->keyed_on);
    }
#undef MIX
    if (ac3_spu_ram) for (u32 i = 0; i < SPU_RAM_SIZE; i += 8) { u64 w; memcpy(&w, ac3_spu_ram + i, 8); x ^= w; x *= 0x100000001B3ull; }
    *h = x;
}

#ifndef AC3_SPU_EAR
int ac3_spu_ear_on, ac3_spu_ear_heard, ac3_spu_ear_mirror;
static FILE *real_wav;   /* AC3_SPU_REAL_WAV: the real chip's mix while the ear is heard (checks) */

void ac3_spu_pump(void) {
    if (!spu_ready) return;
    if (!spu || !spu->frames_wanted) return;
    /* Replays and co-op mix a fixed amount per field: the game reads voice
     * status every frame, so the mix must not follow the audio device's clock
     * (docs/coop/S1_determinism.md). */
    int want = ac3_oracle_deterministic() || ac3_coop_guest ? ac3_spu_field_frames() : spu->frames_wanted();
    while (want > 0) {
        s16 buf[1024 * 2], ear[1024 * 2];
        int n = want > 1024 ? 1024 : want;
        /* the ear following the real chip: copied just before mixing, so
         * sound calls made anywhere in the frame are in it */
        if (ac3_spu_ear_on && ac3_spu_ear_mirror) ac3_spu_ear_follow(SPU_STATE_SETTINGS | SPU_STATE_VOICES | SPU_STATE_RUNNING);
        ac3_spu_core_mix(buf, n);            /* always: the game reads this chip's state */
        const s16 *heard = buf;
        if (ac3_spu_ear_on) {
            ear_ac3_spu_core_mix(ear, n);
            if (ac3_spu_ear_heard) heard = ear;
        }
        if (heard != buf && real_wav) fwrite(buf, 4, (size_t)n, real_wav);
        ac3_spu_dump_write(heard, n);
        if (spu->submit) spu->submit(heard, n);
        want -= n;
    }
}

/* The ear made identical to the real chip: its sound memory, library voice
 * settings and mixer. Afterwards uploads and CD audio reach it as they come. */
void ac3_spu_ear_copy_all(void) {
    spu_ram_init();
    if (!ear_ac3_spu_ram) ear_ac3_spu_ram = (u8 *)calloc(1, EAR_SAMPLE_SPACE);
    if (!ear_ac3_spu_ram) return;
    memcpy(ear_ac3_spu_ram, ac3_spu_ram, SPU_RAM_SIZE);
    ac3_spu_ear_follow(SPU_STATE_ALL);
    if (!ac3_spu_ear_on) {
        const char *w = getenv("AC3_SPU_REAL_WAV");
        if (w && *w && !real_wav && (real_wav = fopen(w, "wb")) != NULL)
            printf("audio: the real sound chip's mix (raw 16-bit stereo 44100 Hz) -> %s\n", w);
    }
    ac3_spu_ear_on = 1;
}

/* The ear's voices (and other parts) made identical to the real chip's. */
void ac3_spu_ear_follow(u32 parts) {
    static u8 *state;
    if (!state) state = (u8 *)malloc(ac3_spu_core_state_size());
    if (!state) return;
    ac3_spu_core_state_save(state);
    ear_ac3_spu_core_state_load(state, parts);
    if (parts & SPU_STATE_VOICES) memcpy(ear_ac3_spu_voice, ac3_spu_voice, sizeof ac3_spu_voice);
}
#else
void ac3_spu_pump(void) { }   /* the ear is mixed by the real chip's pump */
#endif

static const int adpcm_f0[16] = { 0, 60, 115, 98, 122 };
static const int adpcm_f1[16] = { 0, 0, -52, -55, -60 };

u8 ac3_spu_decode_block(const u8 *block, s16 *out28, s16 hist[2]) {
    int shift = block[0] & 0x0F;
    int filter = (block[0] >> 4) & 0x0F;
    u8 flags = block[1];
    int nibble_mask = 0x0F;
    if (shift > 12) { shift = 8; nibble_mask = 0x08; }

    for (int i = 0; i < 28; i++) {
        int nibble = (block[2 + i / 2] >> ((i & 1) ? 4 : 0)) & nibble_mask;
        s32 sample = (s32)(s16)(u16)(nibble << 12) >> shift;
        sample += ((s32)hist[1] * adpcm_f1[filter]) >> 6;
        sample += ((s32)hist[0] * adpcm_f0[filter]) >> 6;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out28[i] = (s16)sample;
        hist[1] = hist[0];
        hist[0] = (s16)sample;
    }
    return flags;
}

void psyq_SpuInit(ctx_t *ctx) {
    spu_ram_init();
    memset(ac3_spu_voice, 0, sizeof ac3_spu_voice);
    for (int v = 0; v < SPU_VOICES; v++) {
        ac3_spu_voice[v].pitch = 0x1000;
        ac3_spu_voice[v].volmode_left = SPU_VOICE_DIRECT;
        ac3_spu_voice[v].volmode_right = SPU_VOICE_DIRECT;
    }
    transfer_addr = 0x1010;
    transfer_mode = SPU_TRANSFER_BY_DMA;
    transfer_callback_fn = 0;
    irq_callback_fn = 0;
    irq_addr = 0;
    irq_on = 0;
    spu_ready = 1;
    ac3_spu_core_reset();
    rev_lib_reset();
    hle_trace("SpuInit", "");
    RET(0);
}

void psyq__SpuInit(ctx_t *ctx) { psyq_SpuInit(ctx); }
void psyq__spu_init(ctx_t *ctx) { psyq_SpuInit(ctx); }

void psyq_SpuInitHot(ctx_t *ctx) {
    spu_ram_init();
    for (int v = 0; v < SPU_VOICES; v++) {
        ac3_spu_voice[v].keyed_on = 0;
        ac3_spu_core_key_off(v);
    }
    transfer_addr = 0x1010;
    transfer_mode = SPU_TRANSFER_BY_DMA;
    transfer_callback_fn = 0;
    irq_callback_fn = 0;
    irq_addr = 0;
    irq_on = 0;
    spu_ready = 1;
    ac3_spu_set_mute(0);
    rev_lib_reset();
    hle_trace("SpuInitHot", "");
    RET(0);
}

void psyq_SpuStart(ctx_t *ctx) { spu_ready = 1; RET(0); }

void psyq_SpuQuit(ctx_t *ctx) {
    spu_ready = 0;
    for (int v = 0; v < SPU_VOICES; v++) ac3_spu_voice[v].keyed_on = 0;
    ac3_spu_core_reset();
    rev_lib_reset();
    RET(0);
}

void psyq_SpuSetMute(ctx_t *ctx) {
    long prev = ac3_spu_get_mute();
    ac3_spu_set_mute((int)A0);
    RET((u32)prev);
}
void psyq_SpuGetMute(ctx_t *ctx) { RET((u32)ac3_spu_get_mute()); }

void psyq_SpuSetTransferMode(ctx_t *ctx) {
    transfer_mode = A0;
    RET((u32)transfer_mode);
}
void psyq_SpuGetTransferMode(ctx_t *ctx) { RET((u32)transfer_mode); }

void psyq_SpuSetTransferStartAddr(ctx_t *ctx) {
    u32 addr = A0 & ~7u;
    if (addr >= SPU_RAM_SIZE) {
        hle_warn("SpuSetTransferStartAddr", "0x%08X out of range", A0);
        RET(0);
        return;
    }
    transfer_addr = addr;
    RET(addr);
}

void psyq_SpuGetTransferStartAddr(ctx_t *ctx) { RET(transfer_addr); }

static void transfer_done(ctx_t *ctx, u32 result) {
    if (transfer_callback_fn) ac3_call_indirect(ctx, transfer_callback_fn);
    RET(result);
}

void psyq_SpuWrite(ctx_t *ctx) {
    spu_ram_init();
    u32 src = A0, size = A1;
    if (transfer_addr + size > SPU_RAM_SIZE) {
        hle_warn("SpuWrite", "transfer of %u at 0x%X overruns SPU RAM", size,
                 transfer_addr);
        size = SPU_RAM_SIZE - transfer_addr;
    }
    const u8 *g = (const u8 *)guest_ptr(src, size);
    if (!g) {
        hle_warn("SpuWrite", "bad source 0x%08X", src);
        RET(0);
        return;
    }
    memcpy(ac3_spu_ram + transfer_addr, g, size);
#ifndef AC3_SPU_EAR
    if (ac3_spu_ear_on) memcpy(ear_ac3_spu_ram + transfer_addr, g, size);   /* samples: the ear has them too */
#endif
    if (spu->upload) spu->upload(transfer_addr, g, size);
    hle_trace("SpuWrite", "%u bytes -> SPU 0x%05X", size, transfer_addr);
    transfer_addr += size;
    transfer_done(ctx, size);
}

void psyq_SpuWritePartly(ctx_t *ctx) { psyq_SpuWrite(ctx); }

void psyq_SpuWrite0(ctx_t *ctx) {
    spu_ram_init();
    u32 size = A0;
    if (transfer_addr + size > SPU_RAM_SIZE) size = SPU_RAM_SIZE - transfer_addr;
    memset(ac3_spu_ram + transfer_addr, 0, size);
#ifndef AC3_SPU_EAR
    if (ac3_spu_ear_on) memset(ear_ac3_spu_ram + transfer_addr, 0, size);
#endif
    transfer_addr += size;
    transfer_done(ctx, size);
}

void psyq_SpuRead(ctx_t *ctx) {
    spu_ram_init();
    u32 dst = A0, size = A1;
    if (transfer_addr + size > SPU_RAM_SIZE) size = SPU_RAM_SIZE - transfer_addr;
    u8 *g = (u8 *)guest_ptr(dst, size);
    if (g) memcpy(g, ac3_spu_ram + transfer_addr, size);
    transfer_addr += size;
    transfer_done(ctx, size);
}

void psyq_SpuIsTransferCompleted(ctx_t *ctx) { RET(1); }

void psyq_SpuSetTransferCallback(ctx_t *ctx) {
    u32 prev = transfer_callback_fn;
    transfer_callback_fn = A0;
    hle_trace("SpuSetTransferCallback", "fn=0x%08X", A0);
    RET(prev);
}

void psyq_SpuReadDecodedData(ctx_t *ctx) {
    spu_ram_init();
    u32 dst = A0;
    long flag = (long)A1;
    if (!dst) { RET(0); return; }
    u8 *g = (u8 *)guest_ptr(dst, 0x1000u);
    if (!g) {
        hle_warn("SpuReadDecodedData", "bad destination 0x%08X", dst);
        RET(0);
        return;
    }
    int want_cd = (flag != SPU_LIB_VOICEONLY);
    int want_voice = (flag != SPU_LIB_CDONLY);
    if (want_cd) memcpy(g, ac3_spu_ram, 0x800u);
    if (want_voice) memcpy(g + 0x800u, ac3_spu_ram + 0x800u, 0x800u);
    transfer_done(ctx, 0);
}

void psyq__SpuDataCallback(ctx_t *ctx) { RET(0); }
void psyq__spu_FiDMA(ctx_t *ctx) { RET(0); }

/* ---- noise, pitch LFO, interrupts, and calls with nothing to do ------------ */

void psyq_SpuSetNoiseVoice(ctx_t *ctx) {
    u32 next = apply_voice_switch(ac3_spu_get_noise_voices(), (long)A0, A1);
    ac3_spu_set_noise_voices(next);
    RET(next);
}
void psyq_SpuGetNoiseVoice(ctx_t *ctx) { RET(ac3_spu_get_noise_voices()); }
void psyq_SpuSetNoiseClock(ctx_t *ctx) {
    ac3_spu_set_noise_clock((int)A0);
    RET((u32)ac3_spu_get_noise_clock());
}
void psyq_SpuGetNoiseClock(ctx_t *ctx) { RET((u32)ac3_spu_get_noise_clock()); }

void psyq_SpuSetPitchLFOVoice(ctx_t *ctx) {
    u32 next = apply_voice_switch(ac3_spu_get_pitch_lfo_voices(), (long)A0, A1);
    ac3_spu_set_pitch_lfo_voices(next);
    RET(next);
}
void psyq_SpuGetPitchLFOVoice(ctx_t *ctx) { RET(ac3_spu_get_pitch_lfo_voices()); }

void psyq_SpuSetIRQ(ctx_t *ctx) {
    int prev = irq_on;
    irq_on = ((long)A0 == SPU_LIB_ON);
    if (irq_on) hle_warn("SpuSetIRQ", "SPU interrupts are not delivered");
    RET((u32)(prev ? SPU_LIB_ON : SPU_LIB_OFF));
}
void psyq_SpuGetIRQ(ctx_t *ctx) { RET((u32)(irq_on ? SPU_LIB_ON : SPU_LIB_OFF)); }
void psyq_SpuSetIRQAddr(ctx_t *ctx) { irq_addr = A0 & ~7u; RET(irq_addr); }
void psyq_SpuGetIRQAddr(ctx_t *ctx) { RET(irq_addr); }
void psyq_SpuSetIRQCallback(ctx_t *ctx) {
    u32 prev = irq_callback_fn;
    irq_callback_fn = A0;
    RET(prev);
}

void psyq_SpuFlush(ctx_t *ctx) { RET(0); }
void psyq_SpuSetEnv(ctx_t *ctx) { RET(0); }

void psyq__SpuIsInAllocateArea(ctx_t *ctx)  { RET(0); }
void psyq__SpuIsInAllocateArea_(ctx_t *ctx) { RET((u32)rev_area_allocated(A0)); }
void psyq__spu_t(ctx_t *ctx)                { RET(0); }

void psyq_SpuStGetVoiceStatus(ctx_t *ctx) { RET(0); }
void psyq_SpuStGetStatus(ctx_t *ctx)      { RET(SPU_ST_STOP_LIB); }

void psyq__spu_getInTransfer(ctx_t *ctx) { RET(0); }
void psyq__spu_setInTransfer(ctx_t *ctx) { RET(0); }
void psyq__spu_gcSPU(ctx_t *ctx)         { RET(0); }

void psyq__spu_FsetRXX(ctx_t *ctx) {
    u32 reg = A0, v = A2 ? (A1 >> 3) : A1;
    s16 l, r;
    switch (reg) {
    case 0xD1: ac3_spu_rvb_set_base((u16)v); break;
    case 0xC2: ac3_spu_rvb_volume(NULL, &r); ac3_spu_rvb_set_volume((s16)v, r); break;
    case 0xC3: ac3_spu_rvb_volume(&l, NULL); ac3_spu_rvb_set_volume(l, (s16)v); break;
    default: break;
    }
    RET(0x1F801C00u + 2u * reg);
}

void psyq__spu_Fw(ctx_t *ctx)       { RET(0); }
void psyq__spu_Fw1ts(ctx_t *ctx)    { RET(0); }
void psyq__spu_Fr(ctx_t *ctx)       { RET(0); }
void psyq__spu_Fr_(ctx_t *ctx)      { RET(0); }
void psyq__spu_FsetPCR(ctx_t *ctx)  { RET(0); }
void psyq__spu_FsetRXXa(ctx_t *ctx) { RET(0); }
void psyq__spu_FgetRXXa(ctx_t *ctx) { RET(0); }

void ac3_spu_machine_save(FILE *f) {
    ac3_ms_tag(f, "SPUL");
    for (int v = 0; v < SPU_VOICES; v++) {
        const spu_voice_t *sv = &ac3_spu_voice[v];
        ac3_ms_u32(f, sv->start_addr);
        ac3_ms_u32(f, sv->loop_addr);
        ac3_ms_u32(f, sv->pitch);
        ac3_ms_u32(f, sv->note);
        ac3_ms_u32(f, sv->sample_note);
        ac3_ms_u32(f, (u32)(s32)sv->vol_left);
        ac3_ms_u32(f, (u32)(s32)sv->vol_right);
        ac3_ms_u32(f, (u32)(s32)sv->volmode_left);
        ac3_ms_u32(f, (u32)(s32)sv->volmode_right);
        ac3_ms_u32(f, sv->adsr1);
        ac3_ms_u32(f, sv->adsr2);
        ac3_ms_u32(f, (u32)sv->keyed_on);
    }
    ac3_ms_u32(f, transfer_addr);
    ac3_ms_u32(f, transfer_mode);
    ac3_ms_u32(f, (u32)spu_ready);
    ac3_ms_u32(f, transfer_callback_fn);
    ac3_ms_u32(f, irq_callback_fn);
    ac3_ms_u32(f, irq_addr);
    ac3_ms_u32(f, (u32)irq_on);
    voice_machine_save(f);
    reverb_machine_save(f);
    ac3_ms_tag(f, "SPUR");
    if (ac3_spu_ram) ac3_ms_bytes(f, ac3_spu_ram, SPU_RAM_SIZE);
    else ac3_ms_bytes(f, NULL, 0);
}
