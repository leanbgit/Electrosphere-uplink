/*
 * psyq_spu_reverb.c -- sound memory allocation (SpuInitMalloc, SpuMalloc,
 * SpuFree) and the reverb (libspu): its modes, work area, depth, delay and
 * feedback, set on the emulated chip (spu_core.c).
 */
#include "psyq_spu_internal.h"
#include "spu_ear.h"
#include "ac3_oracle.h"
#include <string.h>

/* SpuReverbAttr: field offsets, and the mask bits saying which are set */
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

int rev_area_allocated(u32 start_units) {
    u32 a = (start_units * 8u) & (SPU_RAM_SIZE - 1u);
    return block_overlaps(a, SPU_RAM_SIZE - a);
}

void rev_lib_reset(void) {
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
#ifndef AC3_SPU_EAR
    if (ac3_spu_ear_on) memset(ear_ac3_spu_ram + start, 0, SPU_RAM_SIZE - start);
#endif
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

void reverb_machine_save(FILE *f) {
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
}
