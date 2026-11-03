/*
 * psyq_spu_voice.c -- the sound library's voices (libspu): their settings
 * (SpuSetVoiceAttr and friends, read from the game's SpuVoiceAttr), keys on
 * and off, envelope status; and the master and CD volumes (SpuSetCommonAttr).
 */
#include "psyq_spu_internal.h"
#include "ac3_oracle.h"
#include <string.h>

/* SpuVoiceAttr: field offsets, and the mask bits saying which are set */
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

/* SpuCommonAttr */
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

#define SPU_BIT  8          /* apply_voice_switch: set exactly these voices */

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

u32 apply_voice_switch(u32 cur, long on_off, u32 bits) {
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

void voice_machine_save(FILE *f) {
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)mvol[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)mvolmode[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)cdvol[i]);
}
