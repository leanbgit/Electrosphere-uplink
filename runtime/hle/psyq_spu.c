#include "spu_backend.h"
#include "spu_core.h"
#include "ac3_oracle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

u8 *ac3_spu_ram = NULL;
spu_voice_t ac3_spu_voice[SPU_VOICES];

#define VA_VOICE        0
#define VA_MASK         4
#define VA_VOL_L        8
#define VA_VOL_R       10
#define VA_VOLMODE_L   12
#define VA_VOLMODE_R   14
#define VA_VOLX_L      16
#define VA_VOLX_R      18
#define VA_PITCH       20
#define VA_NOTE        22
#define VA_SAMPLE_NOTE 24
#define VA_ENVX        26
#define VA_ADDR        28
#define VA_LOOP_ADDR   32
#define VA_A_MODE      36
#define VA_S_MODE      40
#define VA_R_MODE      44
#define VA_AR          48
#define VA_DR          50
#define VA_SR          52
#define VA_RR          54
#define VA_SL          56
#define VA_ADSR1       58
#define VA_ADSR2       60
#define VA_SIZE        64

#define SPU_VOICE_VOLL        (1u <<  0)
#define SPU_VOICE_VOLR        (1u <<  1)
#define SPU_VOICE_VOLMODEL    (1u <<  2)
#define SPU_VOICE_VOLMODER    (1u <<  3)
#define SPU_VOICE_PITCH       (1u <<  4)
#define SPU_VOICE_NOTE        (1u <<  5)
#define SPU_VOICE_SAMPLE_NOTE (1u <<  6)
#define SPU_VOICE_WDSA        (1u <<  7)
#define SPU_VOICE_ADSR_AMODE  (1u <<  8)
#define SPU_VOICE_ADSR_SMODE  (1u <<  9)
#define SPU_VOICE_ADSR_RMODE  (1u << 10)
#define SPU_VOICE_ADSR_AR     (1u << 11)
#define SPU_VOICE_ADSR_DR     (1u << 12)
#define SPU_VOICE_ADSR_SR     (1u << 13)
#define SPU_VOICE_ADSR_RR     (1u << 14)
#define SPU_VOICE_ADSR_SL     (1u << 15)
#define SPU_VOICE_LSAX        (1u << 16)
#define SPU_VOICE_ADSR_ADSR1  (1u << 17)
#define SPU_VOICE_ADSR_ADSR2  (1u << 18)

#define CA_MASK         0
#define CA_MVOL_L       4
#define CA_MVOL_R       6
#define CA_MVOLMODE_L   8
#define CA_MVOLMODE_R  10
#define CA_MVOLX_L     12
#define CA_MVOLX_R     14
#define CA_CD_VOL_L    16
#define CA_CD_VOL_R    18
#define CA_CD_REVERB   20
#define CA_CD_MIX      24
#define CA_EXT_VOL_L   28
#define CA_EXT_VOL_R   30
#define CA_EXT_REVERB  32
#define CA_EXT_MIX     36

#define SPU_COMMON_MVOLL     (1u <<  0)
#define SPU_COMMON_MVOLR     (1u <<  1)
#define SPU_COMMON_MVOLMODEL (1u <<  2)
#define SPU_COMMON_MVOLMODER (1u <<  3)
#define SPU_COMMON_RVOLL     (1u <<  4)
#define SPU_COMMON_RVOLR     (1u <<  5)
#define SPU_COMMON_CDVOLL    (1u <<  6)
#define SPU_COMMON_CDVOLR    (1u <<  7)
#define SPU_COMMON_CDREV     (1u <<  8)
#define SPU_COMMON_CDMIX     (1u <<  9)
#define SPU_COMMON_EXTVOLL   (1u << 10)
#define SPU_COMMON_EXTVOLR   (1u << 11)
#define SPU_COMMON_EXTREV    (1u << 12)
#define SPU_COMMON_EXTMIX    (1u << 13)

#define RA_MASK      0
#define RA_MODE      4
#define RA_DEPTH_L   8
#define RA_DEPTH_R  10
#define RA_DELAY    12
#define RA_FEEDBACK 16

#define SPU_REV_MODE      (1u << 0)
#define SPU_REV_DEPTHL    (1u << 1)
#define SPU_REV_DEPTHR    (1u << 2)
#define SPU_REV_DELAYTIME (1u << 3)
#define SPU_REV_FEEDBACK  (1u << 4)

#define SPU_VOICE_DIRECT      0
#define SPU_VOICE_LINEARIncN  1
#define SPU_VOICE_LINEARIncR  2
#define SPU_VOICE_LINEARDecN  3
#define SPU_VOICE_LINEARDecR  4
#define SPU_VOICE_EXPIncN     5
#define SPU_VOICE_EXPIncR     6
#define SPU_VOICE_EXPDec      7

#define SPU_TRANSFER_BY_DMA  0
#define SPU_TRANSFER_PEEK    0
#define SPU_TRANSFER_WAIT    1

#define SPU_BIT  8

static void rev_lib_reset(void);
static int rev_area_allocated(u32 start_units);

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

#ifdef AC3_COOP_PROBE
/* coop_probe.c: hash of the SPU state the game can observe (RAM + voices) */
void ac3_spu_state_hash(u64 *h) {
    u64 x = 0xCBF29CE484222325ull;
    const u8 *b = (const u8 *)ac3_spu_voice;
    for (size_t i = 0; i < sizeof ac3_spu_voice; i++) { x ^= b[i]; x *= 0x100000001B3ull; }
    if (ac3_spu_ram) for (u32 i = 0; i < SPU_RAM_SIZE; i += 8) { u64 w; memcpy(&w, ac3_spu_ram + i, 8); x ^= w; x *= 0x100000001B3ull; }
    *h = x;
}
#endif

void ac3_spu_pump(void) {
    if (!spu_ready) return;
    if (!spu || !spu->frames_wanted) return;
    int want = ac3_oracle_deterministic() ? ac3_spu_field_frames() : spu->frames_wanted();
    while (want > 0) {
        s16 buf[1024 * 2];
        int n = want > 1024 ? 1024 : want;
        ac3_spu_core_mix(buf, n);
        ac3_spu_dump_write(buf, n);
        if (spu->submit) spu->submit(buf, n);
        want -= n;
    }
}

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

static int mode_is_exponential(long mode) {
    return mode == SPU_VOICE_EXPIncN || mode == SPU_VOICE_EXPIncR ||
           mode == SPU_VOICE_EXPDec;
}
static int mode_is_decreasing(long mode) {
    return mode == SPU_VOICE_LINEARDecN || mode == SPU_VOICE_LINEARDecR ||
           mode == SPU_VOICE_EXPDec;
}

static void set_a_mode(spu_voice_t *sv, long mode) {
    int expo = mode_is_exponential(mode);
    sv->adsr1 = (u16)((sv->adsr1 & 0x7FFFu) | ((u16)expo << 15));
}
static void set_s_mode(spu_voice_t *sv, long mode) {
    int expo = mode_is_exponential(mode);
    int dec  = mode_is_decreasing(mode);
    sv->adsr2 = (u16)((sv->adsr2 & 0x3FFFu) |
                      ((u16)expo << 15) | ((u16)dec << 14));
}
static void set_r_mode(spu_voice_t *sv, long mode) {
    int expo = (mode == SPU_VOICE_EXPDec);
    sv->adsr2 = (u16)((sv->adsr2 & ~0x20u) | ((u16)expo << 5));
}

static void apply_voice_attr(u32 p, int v, spu_voice_t *sv, u32 mask) {
    u32 all = (mask == 0);

    if (all || (mask & SPU_VOICE_VOLL))
        sv->vol_left = (s16)MEM_R16U(p + VA_VOL_L);
    if (all || (mask & SPU_VOICE_VOLR))
        sv->vol_right = (s16)MEM_R16U(p + VA_VOL_R);
    if (all || (mask & SPU_VOICE_VOLMODEL))
        sv->volmode_left = (s16)MEM_R16U(p + VA_VOLMODE_L);
    if (all || (mask & SPU_VOICE_VOLMODER))
        sv->volmode_right = (s16)MEM_R16U(p + VA_VOLMODE_R);

    if (all || (mask & SPU_VOICE_PITCH))
        sv->pitch = (u16)MEM_R16U(p + VA_PITCH);
    if (all || (mask & SPU_VOICE_SAMPLE_NOTE))
        sv->sample_note = (u16)MEM_R16U(p + VA_SAMPLE_NOTE);
    if (all || (mask & SPU_VOICE_NOTE)) {
        sv->note = (u16)MEM_R16U(p + VA_NOTE);
        sv->pitch = ac3_spu_note_to_pitch(sv->note, sv->sample_note);
    }

    if (all || (mask & SPU_VOICE_WDSA))
        sv->start_addr = MEM_R32(p + VA_ADDR);
    if (all || (mask & SPU_VOICE_LSAX)) {
        sv->loop_addr = MEM_R32(p + VA_LOOP_ADDR);
        ac3_spu_core_set_loop_addr(v, sv->loop_addr);
    }

    if (all || (mask & SPU_VOICE_ADSR_AMODE))
        set_a_mode(sv, (long)MEM_R32(p + VA_A_MODE));
    if (all || (mask & SPU_VOICE_ADSR_SMODE))
        set_s_mode(sv, (long)MEM_R32(p + VA_S_MODE));
    if (all || (mask & SPU_VOICE_ADSR_RMODE))
        set_r_mode(sv, (long)MEM_R32(p + VA_R_MODE));

    if (all || (mask & SPU_VOICE_ADSR_AR))
        sv->adsr1 = (u16)((sv->adsr1 & ~0x7F00u) |
                          ((MEM_R16U(p + VA_AR) & 0x7Fu) << 8));
    if (all || (mask & SPU_VOICE_ADSR_DR))
        sv->adsr1 = (u16)((sv->adsr1 & ~0x00F0u) |
                          ((MEM_R16U(p + VA_DR) & 0x0Fu) << 4));
    if (all || (mask & SPU_VOICE_ADSR_SL))
        sv->adsr1 = (u16)((sv->adsr1 & ~0x000Fu) |
                          (MEM_R16U(p + VA_SL) & 0x0Fu));
    if (all || (mask & SPU_VOICE_ADSR_SR))
        sv->adsr2 = (u16)((sv->adsr2 & ~0x1FC0u) |
                          ((MEM_R16U(p + VA_SR) & 0x7Fu) << 6));
    if (all || (mask & SPU_VOICE_ADSR_RR))
        sv->adsr2 = (u16)((sv->adsr2 & ~0x001Fu) |
                          (MEM_R16U(p + VA_RR) & 0x1Fu));

    if (all || (mask & SPU_VOICE_ADSR_ADSR1))
        sv->adsr1 = (u16)MEM_R16U(p + VA_ADSR1);
    if (all || (mask & SPU_VOICE_ADSR_ADSR2))
        sv->adsr2 = (u16)MEM_R16U(p + VA_ADSR2);
}

void psyq_SpuSetVoiceAttr(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    u32 voices = MEM_R32(p + VA_VOICE);
    u32 mask   = MEM_R32(p + VA_MASK);

    for (int v = 0; v < SPU_VOICES; v++) {
        if (!(voices & (1u << v))) continue;
        apply_voice_attr(p, v, &ac3_spu_voice[v], mask);
        ac3_spu_core_attr_changed(v);
    }
    hle_trace("SpuSetVoiceAttr", "voices=0x%06X mask=0x%05X pitch=%u addr=0x%05X",
              voices, mask,
              (voices & 1u) ? ac3_spu_voice[0].pitch : 0,
              (voices & 1u) ? ac3_spu_voice[0].start_addr : 0);
    RET(0);
}

void psyq_SpuNSetVoiceAttr(ctx_t *ctx) {
    int v = (int)A0;
    u32 p = A1;
    if (!p || v < 0 || v >= SPU_VOICES) { RET(0); return; }
    apply_voice_attr(p, v, &ac3_spu_voice[v], MEM_R32(p + VA_MASK));
    ac3_spu_core_attr_changed(v);
    RET(0);
}

void psyq_SpuRSetVoiceAttr(ctx_t *ctx) {
    int lo = (int)A0, hi = (int)A1;
    u32 p = A2;
    if (!p || lo < 0 || hi >= SPU_VOICES || lo > hi) { RET((u32)SPU_INVALID_ARGS); return; }
    u32 mask = MEM_R32(p + VA_MASK);
    for (int v = lo; v <= hi; v++) {
        apply_voice_attr(p, v, &ac3_spu_voice[v], mask);
        ac3_spu_core_attr_changed(v);
    }
    RET(SPU_SUCCESS);
}

static void store_voice_attr(u32 p, int v) {
    const spu_voice_t *sv = &ac3_spu_voice[v];
    s16 vl, vr;
    ac3_spu_core_volx(v, &vl, &vr);
    MEM_W32(p + VA_VOICE, 1u << v);
    MEM_W16(p + VA_VOL_L, (u16)sv->vol_left);
    MEM_W16(p + VA_VOL_R, (u16)sv->vol_right);
    MEM_W16(p + VA_VOLMODE_L, (u16)sv->volmode_left);
    MEM_W16(p + VA_VOLMODE_R, (u16)sv->volmode_right);
    MEM_W16(p + VA_VOLX_L, (u16)vl);
    MEM_W16(p + VA_VOLX_R, (u16)vr);
    MEM_W16(p + VA_PITCH, sv->pitch);
    MEM_W16(p + VA_NOTE, sv->note);
    MEM_W16(p + VA_SAMPLE_NOTE, sv->sample_note);
    MEM_W16(p + VA_ENVX, (u16)ac3_spu_core_envx(v));
    MEM_W32(p + VA_ADDR, sv->start_addr);
    MEM_W32(p + VA_LOOP_ADDR, ac3_spu_core_loop_addr(v));
    MEM_W32(p + VA_A_MODE, SPU_ADSR_AMODE(sv->adsr1) ? SPU_VOICE_EXPIncN
                                                     : SPU_VOICE_LINEARIncN);
    MEM_W32(p + VA_S_MODE, SPU_ADSR_SMODE(sv->adsr2)
                ? (SPU_ADSR_SDIR(sv->adsr2) ? SPU_VOICE_EXPDec : SPU_VOICE_EXPIncN)
                : (SPU_ADSR_SDIR(sv->adsr2) ? SPU_VOICE_LINEARDecN
                                            : SPU_VOICE_LINEARIncN));
    MEM_W32(p + VA_R_MODE, SPU_ADSR_RMODE(sv->adsr2) ? SPU_VOICE_EXPDec
                                                     : SPU_VOICE_LINEARDecN);
    MEM_W16(p + VA_AR, (u16)SPU_ADSR_AR(sv->adsr1));
    MEM_W16(p + VA_DR, (u16)SPU_ADSR_DR(sv->adsr1));
    MEM_W16(p + VA_SR, (u16)SPU_ADSR_SR(sv->adsr2));
    MEM_W16(p + VA_RR, (u16)SPU_ADSR_RR(sv->adsr2));
    MEM_W16(p + VA_SL, (u16)SPU_ADSR_SL(sv->adsr1));
    MEM_W16(p + VA_ADSR1, sv->adsr1);
    MEM_W16(p + VA_ADSR2, sv->adsr2);
}

void psyq_SpuGetVoiceAttr(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    u32 voices = MEM_R32(p + VA_VOICE);
    for (int v = 0; v < SPU_VOICES; v++)
        if (voices & (1u << v)) { store_voice_attr(p, v); break; }
    RET(0);
}

void psyq_SpuNGetVoiceAttr(ctx_t *ctx) {
    int v = (int)A0;
    if (A1 && v >= 0 && v < SPU_VOICES) store_voice_attr(A1, v);
    RET(0);
}

void psyq__SpuSetAnyVoice(ctx_t *ctx) { RET(A1); }

static u32 apply_voice_switch(u32 cur, long on_off, u32 bits) {
    switch (on_off) {
    case SPU_LIB_ON:  return (cur | bits) & 0xFFFFFFu;
    case SPU_LIB_OFF: return (cur & ~bits) & 0xFFFFFFu;
    case SPU_BIT:     return bits & 0xFFFFFFu;
    default:          return cur;
    }
}

void psyq_SpuSetKey(ctx_t *ctx) {
    long on = (long)A0;
    u32 bits = A1;
    for (int v = 0; v < SPU_VOICES; v++) {
        if (!(bits & (1u << v))) continue;
        if (on == SPU_LIB_ON) {
            ac3_spu_voice[v].keyed_on = 1;
            ac3_spu_core_key_on(v);
            if (spu->key_on) spu->key_on(v, &ac3_spu_voice[v]);
        } else {
            ac3_spu_voice[v].keyed_on = 0;
            ac3_spu_core_key_off(v);
            if (spu->key_off) spu->key_off(v);
        }
    }
    hle_trace("SpuSetKey", "%s 0x%06X", on == SPU_LIB_ON ? "on" : "off", bits);
    RET(0);
}

void psyq_SpuSetKeyOnWithAttr(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    u32 voices = MEM_R32(p + VA_VOICE);
    psyq_SpuSetVoiceAttr(ctx);
    ctx->r[4] = SPU_LIB_ON;
    ctx->r[5] = voices;
    psyq_SpuSetKey(ctx);
    RET(0);
}

void psyq_SpuGetAllKeysStatus(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    for (int v = 0; v < SPU_VOICES; v++)
        MEM_W8(p + (u32)v, (u32)ac3_spu_core_key_status(v));
    RET(0);
}

void psyq_SpuRGetAllKeysStatus(ctx_t *ctx) {
    int lo = (int)A0, hi = (int)A1;
    u32 p = A2;
    if (!p || lo < 0 || hi >= SPU_VOICES || lo > hi) {
        RET((u32)SPU_INVALID_ARGS);
        return;
    }
    for (int v = lo; v <= hi; v++)
        MEM_W8(p + (u32)v, (u32)ac3_spu_core_key_status(v));
    RET(SPU_SUCCESS);
}

void psyq_SpuGetVoiceEnvelope(ctx_t *ctx) {
    int v = (int)A0;
    if (A1 && v >= 0 && v < SPU_VOICES)
        MEM_W16(A1, (u16)ac3_spu_core_envx(v));
    RET(0);
}

void psyq_SpuGetVoiceEnvelopeAttr(ctx_t *ctx) {
    int v = (int)A0;
    if (v < 0 || v >= SPU_VOICES) { RET(0); return; }
    if (A1) MEM_W32(A1, (u32)ac3_spu_core_key_status(v));
    if (A2) MEM_W16(A2, (u16)ac3_spu_core_envx(v));
    RET(0);
}

void psyq_SpuGetKeyStatus(ctx_t *ctx) {
    u32 bits = A0;
    for (int v = 0; v < SPU_VOICES; v++)
        if (bits & (1u << v)) { RET((u32)ac3_spu_core_key_status(v)); return; }
    RET(SPU_ST_OFF);
}

static s16 mvol[2] = { 0x3FFF, 0x3FFF };
static s16 mvolmode[2] = { SPU_VOICE_DIRECT, SPU_VOICE_DIRECT };
static s16 cdvol[2] = { 0x7FFF, 0x7FFF };

void psyq_SpuSetCommonAttr(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    u32 mask = MEM_R32(p + CA_MASK);
    u32 all = (mask == 0);

    if (all || (mask & SPU_COMMON_MVOLL))     mvol[0] = (s16)MEM_R16U(p + CA_MVOL_L);
    if (all || (mask & SPU_COMMON_MVOLR))     mvol[1] = (s16)MEM_R16U(p + CA_MVOL_R);
    if (all || (mask & SPU_COMMON_MVOLMODEL)) mvolmode[0] = (s16)MEM_R16U(p + CA_MVOLMODE_L);
    if (all || (mask & SPU_COMMON_MVOLMODER)) mvolmode[1] = (s16)MEM_R16U(p + CA_MVOLMODE_R);
    ac3_spu_set_master(mvol[0], mvol[1], mvolmode[0], mvolmode[1]);
    if (spu->set_master_volume) spu->set_master_volume(mvol[0], mvol[1]);

    if (all || (mask & SPU_COMMON_CDVOLL)) cdvol[0] = (s16)MEM_R16U(p + CA_CD_VOL_L);
    if (all || (mask & SPU_COMMON_CDVOLR)) cdvol[1] = (s16)MEM_R16U(p + CA_CD_VOL_R);
    ac3_spu_set_cd_volume(cdvol[0], cdvol[1]);
    if (spu->set_cd_volume) spu->set_cd_volume(cdvol[0], cdvol[1]);

    if (all || (mask & SPU_COMMON_CDREV))
        ac3_spu_set_cd_reverb((int)MEM_R32(p + CA_CD_REVERB) == SPU_LIB_ON);
    if (all || (mask & SPU_COMMON_CDMIX))
        ac3_spu_set_cd_mix((int)MEM_R32(p + CA_CD_MIX) == SPU_LIB_ON);

    {
        s16 xl, xr;
        ac3_spu_master_volx(&xl, &xr);
        MEM_W16(p + CA_MVOLX_L, (u16)xl);
        MEM_W16(p + CA_MVOLX_R, (u16)xr);
    }

    hle_trace("SpuSetCommonAttr", "mask=0x%04X master %d,%d cd %d,%d mix=%d",
              mask, mvol[0], mvol[1], cdvol[0], cdvol[1], ac3_spu_get_cd_mix());
    RET(0);
}

void psyq_SpuGetCommonAttr(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    s16 xl, xr;
    ac3_spu_master_volx(&xl, &xr);
    MEM_W16(p + CA_MVOL_L, (u16)mvol[0]);
    MEM_W16(p + CA_MVOL_R, (u16)mvol[1]);
    MEM_W16(p + CA_MVOLMODE_L, (u16)mvolmode[0]);
    MEM_W16(p + CA_MVOLMODE_R, (u16)mvolmode[1]);
    MEM_W16(p + CA_MVOLX_L, (u16)xl);
    MEM_W16(p + CA_MVOLX_R, (u16)xr);
    MEM_W16(p + CA_CD_VOL_L, (u16)cdvol[0]);
    MEM_W16(p + CA_CD_VOL_R, (u16)cdvol[1]);
    MEM_W32(p + CA_CD_MIX, (u32)(ac3_spu_get_cd_mix() ? SPU_LIB_ON : SPU_LIB_OFF));
    RET(0);
}

#define SPU_HEAP_LO   0x01010u
#define SPU_HEAP_HI   SPU_RAM_SIZE
#define SPU_MAX_BLOCKS 64

typedef struct { u32 addr, size; int used; } spu_block_t;
static spu_block_t spu_heap[SPU_MAX_BLOCKS];
static int spu_heap_limit;
static int rev_flag;
static int rev_reserved;
static u32 rev_offsetaddr = 0xFFFE;
static int rev_mode;
static s16 rev_depth[2];
static s32 rev_delay, rev_feedback;

static int block_overlaps(u32 a, u32 size) {
    for (int i = 0; i < SPU_MAX_BLOCKS; i++) {
        if (!spu_heap[i].used) continue;
        if (a < spu_heap[i].addr + spu_heap[i].size && spu_heap[i].addr < a + size)
            return 1;
    }
    return 0;
}

static int reverb_area_free(void) {
    return !block_overlaps(ac3_spu_reverb_work_start(rev_mode),
                           ac3_spu_reverb_work_size(rev_mode));
}

static int reverb_blocks(u32 a, u32 size) {
    if (rev_reserved != SPU_LIB_ON) return 0;
    u32 rbase = ac3_spu_reverb_work_start(rev_mode);
    u32 rsize = ac3_spu_reverb_work_size(rev_mode);
    return (a < rbase + rsize && rbase < a + size);
}

static long spu_heap_alloc(u32 from, u32 size) {
    if (!size) return -1;
    int live = 0;
    for (int i = 0; i < SPU_MAX_BLOCKS; i++) if (spu_heap[i].used) live++;
    if (spu_heap_limit && live >= spu_heap_limit) return -1;

    if (from < SPU_HEAP_LO) from = SPU_HEAP_LO;
    from = (from + 7u) & ~7u;
    for (u32 a = from; a + size <= SPU_HEAP_HI; ) {
        if (block_overlaps(a, size) || reverb_blocks(a, size)) {
            a += 8u;
            continue;
        }
        for (int i = 0; i < SPU_MAX_BLOCKS; i++) {
            if (spu_heap[i].used) continue;
            spu_heap[i].used = 1;
            spu_heap[i].addr = a;
            spu_heap[i].size = size;
            return (long)a;
        }
        return -1;
    }
    return -1;
}

void psyq_SpuInitMalloc(ctx_t *ctx) {
    long num = (long)A0;
    memset(spu_heap, 0, sizeof spu_heap);
    if (num < 0) num = 0;
    if (num > SPU_MAX_BLOCKS) {
        hle_warn("SpuInitMalloc", "%ld blocks requested, %d supported",
                 num, SPU_MAX_BLOCKS);
        num = SPU_MAX_BLOCKS;
    }
    spu_heap_limit = (int)num;
    hle_trace("SpuInitMalloc", "num=%ld table=0x%08X", num, A1);
    RET((u32)num);
}

void psyq_SpuMalloc(ctx_t *ctx) {
    long r = spu_heap_alloc(SPU_HEAP_LO, A0);
    hle_trace("SpuMalloc", "%u bytes -> 0x%05lX", A0, r);
    RET((u32)r);
}

void psyq_SpuMallocWithStartAddr(ctx_t *ctx) {
    long r = spu_heap_alloc(A0, A1);
    hle_trace("SpuMallocWithStartAddr", "0x%05X + %u -> 0x%05lX", A0, A1, r);
    RET((u32)r);
}

void psyq_SpuFree(ctx_t *ctx) {
    u32 a = A0;
    for (int i = 0; i < SPU_MAX_BLOCKS; i++)
        if (spu_heap[i].used && spu_heap[i].addr == a) {
            spu_heap[i].used = 0;
            RET(0);
            return;
        }
    hle_warn("SpuFree", "0x%05X was not allocated", a);
    RET(0);
}

static const u16 rev_start[SPU_REV_MODE_MAX] = {
    0xFFFE, 0xFB28, 0xFC18, 0xF6F8, 0xF204, 0xEA44, 0xE128, 0xCFF8, 0xCFF8, 0xF880,
};

static const u16 rev_preset[SPU_REV_MODE_MAX][32] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
      0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
      0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000 },
    { 0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00,
      0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
      0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
      0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000 },
    { 0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
      0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
      0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
      0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000 },
    { 0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
      0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
      0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
      0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000 },
    { 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
      0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
      0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
      0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000 },
    { 0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000,
      0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
      0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
      0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000 },
    { 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100,
      0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
      0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000 },
    { 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
      0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000 },
    { 0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500,
      0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
      0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
      0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000 },
};

u32 ac3_spu_reverb_work_start(int mode) {
    if (mode <= 0 || mode >= SPU_REV_MODE_MAX) return SPU_RAM_SIZE - 0x80u;
    return (u32)rev_start[mode] * 8u;
}
u32 ac3_spu_reverb_work_size(int mode) {
    if (mode < 0 || mode >= SPU_REV_MODE_MAX) return 0;
    return SPU_RAM_SIZE - ac3_spu_reverb_work_start(mode);
}
const u16 *ac3_spu_reverb_preset(int mode) {
    return (mode >= 0 && mode < SPU_REV_MODE_MAX) ? rev_preset[mode] : NULL;
}

static int rev_area_allocated(u32 start_units) {
    u32 a = (start_units * 8u) & (SPU_RAM_SIZE - 1u);
    return block_overlaps(a, SPU_RAM_SIZE - a);
}

static void rev_lib_reset(void) {
    rev_flag = 0;
    rev_reserved = 0;
    rev_mode = SPU_REV_MODE_OFF;
    rev_depth[0] = rev_depth[1] = 0;
    rev_delay = rev_feedback = 0;
    rev_offsetaddr = 0xFFFE;
    ac3_spu_rvb_set_volume(0, 0);
    ac3_spu_set_reverb_voices(0);
    ac3_spu_rvb_set_enable(0);
    ac3_spu_rvb_set_base((u16)rev_offsetaddr);
}

static void set_reverb_attr(u32 mask, const u16 *regs) {
    for (int i = 0; i < 32; i++)
        if (!mask || (mask & (1u << i))) ac3_spu_rvb_set_reg(i, regs[i]);
}

void psyq__spu_setReverbAttr(ctx_t *ctx) {
    u32 p = A0;
    u16 regs[32];
    for (int i = 0; i < 32; i++) regs[i] = (u16)MEM_R16U(p + 4u + 2u * (u32)i);
    set_reverb_attr(MEM_R32(p), regs);
    RET(0);
}

static int clear_work_area(u32 mode) {
    if (mode >= SPU_REV_MODE_MAX) return SPU_ERROR;
    if (rev_area_allocated(rev_start[mode])) return SPU_ERROR;
    u32 start = ac3_spu_reverb_work_start((int)mode);
    if (ac3_spu_ram) memset(ac3_spu_ram + start, 0, SPU_RAM_SIZE - start);
    return 0;
}

void psyq_SpuClearReverbWorkArea(ctx_t *ctx) {
    int r = clear_work_area(A0);
    if (r) hle_warn("SpuClearReverbWorkArea", "mode %ld refused", (long)A0);
    RET((u32)r);
}

void psyq_SpuSetReverb(ctx_t *ctx) {
    long on = (long)A0;
    if (on == SPU_LIB_OFF) {
        rev_flag = 0;
        ac3_spu_rvb_set_enable(0);
    } else if (on == SPU_LIB_ON) {
        if (rev_reserved != SPU_LIB_ON && rev_area_allocated(rev_offsetaddr)) {
            hle_warn("SpuSetReverb", "work area is allocated; reverb stays off");
            rev_flag = 0;
            ac3_spu_rvb_set_enable(0);
        } else {
            rev_flag = 1;
            ac3_spu_rvb_set_enable(1);
        }
    }
    RET((u32)rev_flag);
}

void psyq_SpuGetReverb(ctx_t *ctx) { RET((u32)rev_flag); }

void psyq_SpuSetReverbModeParam(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET((u32)SPU_ERROR); return; }
    u32 mask = MEM_R32(p + RA_MASK);
    int all = (mask == 0);
    int mode_set = 0, delay_set = 0, fb_set = 0, clear = 0;
    u32 regmask = 0;
    u16 regs[32];
    memset(regs, 0, sizeof regs);

    if (all || (mask & SPU_REV_MODE)) {
        u32 mode = MEM_R32(p + RA_MODE);
        if (mode & SPU_REV_MODE_CLEAR_WA) { mode &= ~(u32)SPU_REV_MODE_CLEAR_WA; clear = 1; }
        if (mode >= SPU_REV_MODE_MAX || rev_area_allocated(rev_start[mode])) {
            RET((u32)SPU_ERROR);
            return;
        }
        mode_set = 1;
        rev_mode = (int)mode;
        rev_offsetaddr = rev_start[mode];
        memcpy(regs, rev_preset[mode], sizeof regs);
        if (mode == SPU_REV_MODE_ECHO)       { rev_delay = 0x7F; rev_feedback = 0x7F; }
        else if (mode == SPU_REV_MODE_DELAY) { rev_delay = 0x7F; rev_feedback = 0; }
        else                                 { rev_delay = 0; rev_feedback = 0; }
    }

    int echo = (rev_mode == SPU_REV_MODE_ECHO || rev_mode == SPU_REV_MODE_DELAY);

    if ((all || (mask & SPU_REV_DELAYTIME)) && echo) {
        if (!mode_set) {
            delay_set = 1;
            memcpy(regs, rev_preset[rev_mode], sizeof regs);
            regmask = 0x0C011C00u;
        }
        s32 d = (s32)MEM_R32(p + RA_DELAY);
        rev_delay = d;
        s32 d13 = (s32)((u32)d << 13) / 127;
        s32 d12 = (s32)((u32)d << 12) / 127;
        regs[10] = (u16)(d13 - regs[0]);
        regs[11] = (u16)(d12 - regs[1]);
        regs[16] = (u16)(regs[17] + d12);
        regs[12] = (u16)(regs[13] + d12);
        regs[27] = (u16)(regs[29] + d12);
        regs[26] = (u16)(regs[28] + d12);
    }

    if ((all || (mask & SPU_REV_FEEDBACK)) && echo) {
        fb_set = 1;
        if (!mode_set) {
            if (!delay_set) {
                memcpy(regs, rev_preset[rev_mode], sizeof regs);
                regmask = 0x80u;
            } else {
                regmask |= 0x80u;
            }
        }
        s32 fb = (s32)MEM_R32(p + RA_FEEDBACK);
        rev_feedback = fb;
        regs[7] = (u16)((s32)((u32)fb * 0x8100u) / 127);
    }

    int was_on = 0;
    if (mode_set) {
        was_on = ac3_spu_rvb_enabled();
        if (was_on) ac3_spu_rvb_set_enable(0);
        ac3_spu_rvb_set_volume(0, 0);
        rev_depth[0] = rev_depth[1] = 0;
    } else {
        if (all || (mask & SPU_REV_DEPTHL)) rev_depth[0] = (s16)MEM_R16U(p + RA_DEPTH_L);
        if (all || (mask & SPU_REV_DEPTHR)) rev_depth[1] = (s16)MEM_R16U(p + RA_DEPTH_R);
        s16 l, r;
        ac3_spu_rvb_volume(&l, &r);
        if (all || (mask & SPU_REV_DEPTHL)) l = rev_depth[0];
        if (all || (mask & SPU_REV_DEPTHR)) r = rev_depth[1];
        ac3_spu_rvb_set_volume(l, r);
    }

    if (mode_set || delay_set || fb_set) set_reverb_attr(regmask, regs);
    if (clear) clear_work_area((u32)rev_mode);
    if (mode_set) {
        ac3_spu_rvb_set_base((u16)rev_offsetaddr);
        if (was_on) ac3_spu_rvb_set_enable(1);
    }
    hle_trace("SpuSetReverbModeParam", "mask=0x%02X mode=%d delay=%d fb=%d",
              mask, rev_mode, rev_delay, rev_feedback);
    RET(0);
}

void psyq_SpuGetReverbModeParam(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    MEM_W32(p + RA_MODE, (u32)rev_mode);
    MEM_W16(p + RA_DEPTH_L, (u16)rev_depth[0]);
    MEM_W16(p + RA_DEPTH_R, (u16)rev_depth[1]);
    MEM_W32(p + RA_DELAY, (u32)rev_delay);
    MEM_W32(p + RA_FEEDBACK, (u32)rev_feedback);
    RET(0);
}

void psyq_SpuSetReverbDepth(ctx_t *ctx) {
    u32 p = A0;
    if (!p) { RET(0); return; }
    u32 mask = MEM_R32(p + RA_MASK);
    s16 l, r;
    ac3_spu_rvb_volume(&l, &r);
    if (mask == 0 || (mask & SPU_REV_DEPTHL)) l = rev_depth[0] = (s16)MEM_R16U(p + RA_DEPTH_L);
    if (mask == 0 || (mask & SPU_REV_DEPTHR)) r = rev_depth[1] = (s16)MEM_R16U(p + RA_DEPTH_R);
    ac3_spu_rvb_set_volume(l, r);
    hle_trace("SpuSetReverbDepth", "%d,%d", l, r);
    RET(0);
}

void psyq_SpuSetReverbVoice(ctx_t *ctx) {
    u32 next = apply_voice_switch(ac3_spu_get_reverb_voices(), (long)A0, A1);
    ac3_spu_set_reverb_voices(next);
    hle_trace("SpuSetReverbVoice", "on_off=%ld bits=0x%06X -> 0x%06X",
              (long)A0, A1, next);
    RET(next);
}

void psyq_SpuGetReverbVoice(ctx_t *ctx) { RET(ac3_spu_get_reverb_voices()); }

void psyq_SpuReserveReverbWorkArea(ctx_t *ctx) {
    long on = (long)A0;
    if (on == SPU_LIB_ON) {
        if (!reverb_area_free()) {
            hle_warn("SpuReserveReverbWorkArea",
                     "work area already allocated by SpuMalloc");
            RET((u32)SPU_LIB_OFF);
            return;
        }
        rev_reserved = SPU_LIB_ON;
    } else if (on == SPU_LIB_OFF) {
        rev_reserved = SPU_LIB_OFF;
    }
    RET((u32)on);
}

void psyq_SpuIsReverbWorkAreaReserved(ctx_t *ctx) {
    long on = (long)A0;
    int answer;
    if (on == SPU_DIAG) answer = reverb_area_free();
    else                answer = (rev_reserved == SPU_LIB_ON);
    RET((u32)(answer ? SPU_LIB_ON : SPU_LIB_OFF));
}

void psyq_SpuSetESA(ctx_t *ctx) {
    rev_offsetaddr = (A0 >> 3) & 0xFFFFu;
    ac3_spu_rvb_set_base((u16)rev_offsetaddr);
    RET(0);
}

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
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)mvol[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)mvolmode[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)cdvol[i]);
    for (int i = 0; i < SPU_MAX_BLOCKS; i++) {
        ac3_ms_u32(f, spu_heap[i].addr);
        ac3_ms_u32(f, spu_heap[i].size);
        ac3_ms_u32(f, (u32)spu_heap[i].used);
    }
    ac3_ms_u32(f, (u32)spu_heap_limit);
    ac3_ms_u32(f, (u32)rev_flag);
    ac3_ms_u32(f, (u32)rev_reserved);
    ac3_ms_u32(f, rev_offsetaddr);
    ac3_ms_u32(f, (u32)rev_mode);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)rev_depth[i]);
    ac3_ms_u32(f, (u32)rev_delay);
    ac3_ms_u32(f, (u32)rev_feedback);
    ac3_ms_tag(f, "SPUR");
    if (ac3_spu_ram) ac3_ms_bytes(f, ac3_spu_ram, SPU_RAM_SIZE);
    else ac3_ms_bytes(f, NULL, 0);
}
