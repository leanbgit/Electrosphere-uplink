#include "spu_core.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ac3_oracle.h"

#define VM_DIRECT      0
#define VM_LINEAR_INCN 1
#define VM_LINEAR_INCR 2
#define VM_LINEAR_DECN 3
#define VM_LINEAR_DECR 4
#define VM_EXP_INCN    5
#define VM_EXP_INCR    6
#define VM_EXP_DEC     7

#define VOL_MAX 0x3FFF

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    u32 cur;
    int have_block;
    u8  flags;
    s16 dec[28];
    s16 hist[2];
    int idx;

    u32 counter;
    s16 win[4];
    int ended;

    int phase;
    s32 env;
    u32 env_div;
    s32 out;

    s32 vol[2];
    s32 vol_cyc[2];
} chan_t;

static chan_t chan[SPU_VOICES];

static u32 repeat_addr[SPU_VOICES];
static u8  ignore_loop_start[SPU_VOICES];

static s32 master[2] = { 0x3FFF, 0x3FFF };
static s16 master_mode[2] = { VM_DIRECT, VM_DIRECT };
static s32 master_cur[2] = { 0x3FFF, 0x3FFF };
static s32 master_cyc[2];
static int muted;

static s32 cd_vol[2] = { 0x7FFF, 0x7FFF };
static int cd_reverb_on;
static int cd_mix_on = 1;

#define CD_FIFO_FRAMES 32768
static s16 cd_fifo[CD_FIFO_FRAMES * 2];
static u32 cd_rd, cd_wr;

static u32 noise_voices, reverb_voices, lfo_voices;
static int noise_clock;
static u16 noise_lfsr = 0x8000;
static s32 noise_timer;

#define DECODED_BASE(which) ((u32)(which) * 0x400u)
static u32 decoded_pos;

static void decoded_write(int which, s16 v) {
    if (!ac3_spu_ram) return;
    u32 off = DECODED_BASE(which) + decoded_pos * 2u;
    if (off + 2u > SPU_RAM_SIZE) return;
    memcpy(ac3_spu_ram + off, &v, 2);
}

static u32 out_peak;
static u64 out_frames;
static u32 voices_played;

static s32 clamp16(s32 v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static u32 field_acc;

int ac3_spu_field_frames(void) {
    field_acc += SPU_FIELD_NUM;
    int n = (int)(field_acc / SPU_FIELD_DEN);
    field_acc %= SPU_FIELD_DEN;
    return n;
}

typedef struct { s32 step, cycles; } rate_t;

static rate_t rate_of(int rate, int increase, int expo, s32 level) {
    rate_t o;
    int shift = (rate >> 2) & 31;
    int sel = rate & 3;
    s32 step = increase ? (7 - sel) : (-8 + sel);

    o.cycles = 1;
    if (shift > 11) o.cycles = 1 << (shift - 11);
    else step <<= (11 - shift);

    if (expo) {
        if (increase) {
            if (level > 0x6000) o.cycles *= 4;
        } else {
            step = (s32)(((s64)step * (s64)level) >> 15);
            if (step == 0) step = -1;
        }
    }
    o.step = step;
    return o;
}

#define ADPCM_LOOP_END   0x01
#define ADPCM_SUSTAIN    0x02
#define ADPCM_LOOP_START 0x04

#define SPU_ADDR_MASK (SPU_RAM_SIZE - 16u)

static int block_load(chan_t *c, int v) {
    if (!ac3_spu_ram) return 0;
    c->cur &= SPU_ADDR_MASK;
    c->flags = ac3_spu_decode_block(ac3_spu_ram + c->cur, c->dec, c->hist);
    if ((c->flags & ADPCM_LOOP_START) && !ignore_loop_start[v])
        repeat_addr[v] = c->cur;
    c->idx = 0;
    c->have_block = 1;
    return 1;
}

static int voice_pull(chan_t *c, int v) {
    if (!c->have_block || c->idx >= 28) {
        if (c->have_block) {
            if (c->flags & ADPCM_LOOP_END) {
                c->cur = repeat_addr[v];
                if (!(c->flags & ADPCM_SUSTAIN)) {
                    c->ended = 1;
                    c->env = 0;
                    c->phase = ENV_OFF;
                    return 0;
                }
            } else {
                c->cur += 16u;
            }
        }
        if (!block_load(c, v)) { c->ended = 1; c->env = 0; c->phase = ENV_OFF; return 0; }
    }
    c->win[0] = c->win[1];
    c->win[1] = c->win[2];
    c->win[2] = c->win[3];
    c->win[3] = c->dec[c->idx++];
    return 1;
}

static const s16 gauss[512] = {
    #include "spu_gauss.inc"
};

static s32 gauss_interp(const chan_t *c) {
    int i = (int)((c->counter >> 4) & 0xFFu);
    s32 out = (s32)gauss[0x0FF - i] * c->win[0]
            + (s32)gauss[0x1FF - i] * c->win[1]
            + (s32)gauss[0x100 + i] * c->win[2]
            + (s32)gauss[0x000 + i] * c->win[3];
    return out >> 15;
}

static s16 noise_next(void) {
    s32 period = 0x8000 >> (noise_clock >> 2);
    if (period < 1) period = 1;
    noise_timer -= 0x8000;
    while (noise_timer <= 0) {
        noise_timer += period * 0x100;
        u16 bit = (u16)(((noise_lfsr >> 15) ^ (noise_lfsr >> 14) ^
                         (noise_lfsr >> 12) ^ (noise_lfsr >> 3)) & 1u);
        noise_lfsr = (u16)((noise_lfsr << 1) | bit);
    }
    return (s16)noise_lfsr;
}

#define RVB_WORDS 0x40000u

static u16 rvb_reg[32];
static u16 rvb_mbase;
static u32 rvb_wa;
static u32 rvb_cur;
static s32 rvb_vol[2];
static int rvb_enable;
static s16 rvb_down[2][128];
static s16 rvb_up[2][64];
static u32 rvb_pos;

#define R_FB_SRC_A   0
#define R_FB_SRC_B   1
#define R_IIR_ALPHA  2
#define R_ACC_COEF_A 3
#define R_ACC_COEF_B 4
#define R_ACC_COEF_C 5
#define R_ACC_COEF_D 6
#define R_IIR_COEF   7
#define R_FB_ALPHA   8
#define R_FB_X       9
#define R_IIR_DEST_A 10
#define R_ACC_SRC_A  12
#define R_ACC_SRC_B  14
#define R_IIR_SRC_A  16
#define R_IIR_DEST_B 18
#define R_ACC_SRC_C  20
#define R_ACC_SRC_D  22
#define R_IIR_SRC_B  24
#define R_MIX_DEST_A 26
#define R_MIX_DEST_B 28
#define R_IN_COEF    30

#define RVB_S(i) ((s32)(s16)rvb_reg[i])

static s16 rvb_sat(s32 v) {
    return (s16)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

static s32 rvb_neg(s32 v) { return v == -32768 ? 0x7FFF : -v; }

static u32 rvb_addr(u32 in) {
    u32 off = rvb_cur + (in & 0x3FFFFu);
    off += rvb_wa & (0u - ((off >> 18) & 1u));
    return off & 0x3FFFFu;
}

static s16 rvb_rd(u16 reg, s32 extra) {
    if (!ac3_spu_ram) return 0;
    s16 v;
    memcpy(&v, ac3_spu_ram + rvb_addr(((u32)reg << 2) + (u32)extra) * 2u, 2);
    return v;
}

static void rvb_wr(u16 reg, s16 v) {
    if (!rvb_enable || !ac3_spu_ram) return;
    memcpy(ac3_spu_ram + rvb_addr((u32)reg << 2) * 2u, &v, 2);
}

static const s16 rvb_fir[20] = {
    -1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246,
    10246, -2960, 1332, -616, 266, -103, 35, -10, 2, -1,
};

static s32 rvb_downsample(const s16 *src) {
    s32 out = 0;
    for (int i = 0; i < 20; i++) out += rvb_fir[i] * src[i * 2];
    out += 0x4000 * src[19];
    return rvb_sat(out >> 15);
}

static s32 rvb_upsample(const s16 *src) {
    s32 out = 0;
    for (int i = 0; i < 20; i++) out += rvb_fir[i] * src[i];
    return rvb_sat(out >> 14);
}

static s32 rvb_iiasm(s16 alpha, s16 insamp) {
    if (alpha == -32768) return insamp == -32768 ? 0 : insamp * -65536;
    return insamp * (32768 - alpha);
}

static void rvb_side(int lr, s32 in) {
    s32 in_coef = RVB_S(R_IN_COEF + lr);
    s32 iir_coef = RVB_S(R_IIR_COEF);
    s16 alpha = (s16)rvb_reg[R_IIR_ALPHA];

    s16 iir_in_a = rvb_sat((((rvb_rd(rvb_reg[R_IIR_SRC_A + lr], 0) * iir_coef) >> 14) +
                            ((in * in_coef) >> 14)) >> 1);
    s16 iir_in_b = rvb_sat((((rvb_rd(rvb_reg[R_IIR_SRC_B + (lr ^ 1)], 0) * iir_coef) >> 14) +
                            ((in * in_coef) >> 14)) >> 1);
    s16 iir_a = rvb_sat((((iir_in_a * alpha) >> 14) +
                         (rvb_iiasm(alpha, rvb_rd(rvb_reg[R_IIR_DEST_A + lr], -1)) >> 14)) >> 1);
    s16 iir_b = rvb_sat((((iir_in_b * alpha) >> 14) +
                         (rvb_iiasm(alpha, rvb_rd(rvb_reg[R_IIR_DEST_B + lr], -1)) >> 14)) >> 1);
    rvb_wr(rvb_reg[R_IIR_DEST_A + lr], iir_a);
    rvb_wr(rvb_reg[R_IIR_DEST_B + lr], iir_b);

    s32 acc = ((rvb_rd(rvb_reg[R_ACC_SRC_A + lr], 0) * RVB_S(R_ACC_COEF_A)) >> 14) +
              ((rvb_rd(rvb_reg[R_ACC_SRC_B + lr], 0) * RVB_S(R_ACC_COEF_B)) >> 14) +
              ((rvb_rd(rvb_reg[R_ACC_SRC_C + lr], 0) * RVB_S(R_ACC_COEF_C)) >> 14) +
              ((rvb_rd(rvb_reg[R_ACC_SRC_D + lr], 0) * RVB_S(R_ACC_COEF_D)) >> 14);

    s32 fb_alpha = RVB_S(R_FB_ALPHA), fb_x = RVB_S(R_FB_X);
    s16 fb_a = rvb_rd((u16)(rvb_reg[R_MIX_DEST_A + lr] - rvb_reg[R_FB_SRC_A]), 0);
    s16 fb_b = rvb_rd((u16)(rvb_reg[R_MIX_DEST_B + lr] - rvb_reg[R_FB_SRC_B]), 0);
    s16 mda = rvb_sat((acc + ((fb_a * rvb_neg(fb_alpha)) >> 14)) >> 1);
    s16 mdb = rvb_sat(fb_a + ((((mda * fb_alpha) >> 14) +
                                ((fb_b * rvb_neg(fb_x)) >> 14)) >> 1));
    s16 ivb = rvb_sat(fb_b + ((mdb * fb_x) >> 15));
    rvb_wr(rvb_reg[R_MIX_DEST_A + lr], mda);
    rvb_wr(rvb_reg[R_MIX_DEST_B + lr], mdb);

    rvb_up[lr][(rvb_pos >> 1) | 0x20] = rvb_up[lr][rvb_pos >> 1] = ivb;
}

static void rvb_tick(const s32 in[2], s32 out[2]) {
    for (int lr = 0; lr < 2; lr++)
        rvb_down[lr][rvb_pos] = rvb_down[lr][rvb_pos | 0x40] = (s16)in[lr];

    if (rvb_pos & 1) {
        rvb_side(0, rvb_downsample(&rvb_down[0][(rvb_pos - 38) & 0x3F]));
    } else {
        rvb_side(1, rvb_downsample(&rvb_down[1][(rvb_pos - 38) & 0x3F]));
        rvb_cur = (rvb_cur + 1) & 0x3FFFFu;
        if (!rvb_cur) rvb_cur = rvb_wa;
    }

    const s16 *l = &rvb_up[0][((rvb_pos >> 1) - 19) & 0x1F];
    const s16 *r = &rvb_up[1][((rvb_pos >> 1) - 19) & 0x1F];
    if (rvb_pos & 1) { out[0] = rvb_upsample(l); out[1] = r[9]; }
    else             { out[0] = l[9];            out[1] = rvb_upsample(r); }

    rvb_pos = (rvb_pos + 1) & 0x3Fu;
}

static void rvb_reset(void) {
    memset(rvb_reg, 0, sizeof rvb_reg);
    memset(rvb_down, 0, sizeof rvb_down);
    memset(rvb_up, 0, sizeof rvb_up);
    rvb_vol[0] = rvb_vol[1] = 0;
    rvb_enable = 0;
    rvb_pos = 0;
    rvb_mbase = 0;
    rvb_wa = 0;
    rvb_cur = 0;
}

static void env_delta(int zs, int speed, int expo, int dec, int inv, s32 level,
                      s32 *inc, s32 *div_inc) {
    *inc = 7 - (speed & 3);
    if (inv) *inc = ~*inc;
    *div_inc = 32768;
    if (speed < 0x2C) *inc = (s32)((u32)*inc << ((0x2F - speed) >> 2));
    if (speed >= 0x30) *div_inc >>= (speed - 0x2C) >> 2;
    if (expo) {
        if (dec) {
            *inc = (level * *inc) >> 15;
        } else if ((level & 0x7FFF) >= 0x6000) {
            if (speed < 0x28)       *inc >>= 2;
            else if (speed >= 0x2C) *div_inc >>= 2;
            else { *inc >>= 1; *div_inc >>= 1; }
        }
    }
    if (*div_inc == 0 && speed < zs) *div_inc = 1;
}

static void env_tick(chan_t *c, const spu_voice_t *v) {
    if (c->phase == ENV_OFF) return;
    if (c->phase == ENV_ATTACK && c->env == 0x7FFF) c->phase = ENV_DECAY;

    s32 inc = 0, div_inc = 0;
    u16 reset = 0;
    int sdec = (int)SPU_ADSR_SDIR(v->adsr2);
    switch (c->phase) {
    case ENV_ATTACK:
        env_delta(0x7F, (int)SPU_ADSR_AR(v->adsr1), (int)SPU_ADSR_AMODE(v->adsr1),
                  0, 0, c->env, &inc, &div_inc);
        reset = 0x7FFF;
        break;
    case ENV_DECAY:
        env_delta(0x1F << 2, (int)SPU_ADSR_DR(v->adsr1) << 2, 1, 1, 1,
                  c->env, &inc, &div_inc);
        reset = 0;
        break;
    case ENV_SUSTAIN:
        env_delta(0x7F, (int)SPU_ADSR_SR(v->adsr2), (int)SPU_ADSR_SMODE(v->adsr2),
                  sdec, sdec, c->env, &inc, &div_inc);
        reset = sdec ? 0 : 0x7FFF;
        break;
    case ENV_RELEASE:
        env_delta(0x1F << 2, (int)SPU_ADSR_RR(v->adsr2) << 2,
                  (int)SPU_ADSR_RMODE(v->adsr2), 1, 1, c->env, &inc, &div_inc);
        reset = 0;
        break;
    default:
        return;
    }

    c->env_div += (u32)div_inc;
    if (!(c->env_div & 0x8000u)) return;
    c->env_div = 0;

    u16 prev = (u16)c->env;
    u16 next = (u16)(prev + inc);
    if (c->phase == ENV_ATTACK) {
        if ((prev ^ next) & next & 0x8000u) next = reset;
    } else if (next & 0x8000u) {
        next = reset;
    }
    c->env = next;
    if (c->phase == ENV_DECAY &&
        next < (u16)(((s32)SPU_ADSR_SL(v->adsr1) + 1) << 11))
        c->phase = ENV_SUSTAIN;
    if (c->phase == ENV_RELEASE && next == 0) c->phase = ENV_OFF;
}

static void sweep_tick(s32 *level, s32 *cyc, int mode, s32 rate) {
    if (mode == VM_DIRECT) return;
    if (--*cyc > 0) return;

    int increase = (mode == VM_LINEAR_INCN || mode == VM_LINEAR_INCR ||
                    mode == VM_EXP_INCN   || mode == VM_EXP_INCR);
    int expo = (mode >= VM_EXP_INCN);
    int neg = (*level < 0);
    s32 mag = neg ? -*level : *level;

    rate_t r = rate_of((int)(rate & 0x7F), increase, expo, mag * 2);
    mag += r.step;
    if (mag > VOL_MAX) mag = VOL_MAX;
    if (mag < 0) mag = 0;
    *cyc = r.cycles;
    *level = neg ? -mag : mag;
}

void ac3_spu_core_reset(void) {
    memset(chan, 0, sizeof chan);
    memset(repeat_addr, 0, sizeof repeat_addr);
    memset(ignore_loop_start, 0, sizeof ignore_loop_start);
    for (int v = 0; v < SPU_VOICES; v++) {
        chan[v].phase = ENV_OFF;
        chan[v].ended = 1;
    }
    master[0] = master[1] = 0x3FFF;
    master_cur[0] = master_cur[1] = 0x3FFF;
    master_mode[0] = master_mode[1] = VM_DIRECT;
    master_cyc[0] = master_cyc[1] = 1;
    muted = 0;
    cd_vol[0] = cd_vol[1] = 0x7FFF;
    cd_reverb_on = 0;
    cd_mix_on = 1;
    cd_rd = cd_wr = 0;
    noise_voices = reverb_voices = lfo_voices = 0;
    noise_clock = 0;
    noise_lfsr = 0x8000;
    noise_timer = 0;
    rvb_reset();
    if (ac3_spu_ram) memset(ac3_spu_ram, 0, 0x1000u);
    decoded_pos = 0;
    out_peak = 0;
    out_frames = 0;
    voices_played = 0;
}

void ac3_spu_core_key_on(int v) {
    if (v < 0 || v >= SPU_VOICES) return;
    chan_t *c = &chan[v];
    const spu_voice_t *a = &ac3_spu_voice[v];
    memset(c, 0, sizeof *c);
    ignore_loop_start[v] = 0;
    c->cur = a->start_addr & SPU_ADDR_MASK;
    c->phase = ENV_ATTACK;
    c->env = 0;
    c->vol[0] = (a->volmode_left == VM_DIRECT) ? a->vol_left : 0;
    c->vol[1] = (a->volmode_right == VM_DIRECT) ? a->vol_right : 0;
    c->vol_cyc[0] = c->vol_cyc[1] = 1;
    c->ended = 0;
    voice_pull(c, v);
}

void ac3_spu_core_key_off(int v) {
    if (v < 0 || v >= SPU_VOICES) return;
    if (chan[v].phase == ENV_OFF) return;
    chan[v].phase = ENV_RELEASE;
    chan[v].env_div = 0;
}

void ac3_spu_core_set_loop_addr(int v, u32 addr) {
    if (v < 0 || v >= SPU_VOICES) return;
    repeat_addr[v] = addr & SPU_ADDR_MASK;
    if (chan[v].phase != ENV_OFF) ignore_loop_start[v] = 1;
}

u32 ac3_spu_core_loop_addr(int v) {
    return (v >= 0 && v < SPU_VOICES) ? repeat_addr[v] : 0;
}

void ac3_spu_core_attr_changed(int v) {
    if (v < 0 || v >= SPU_VOICES) return;
    const spu_voice_t *a = &ac3_spu_voice[v];
    if (a->volmode_left == VM_DIRECT) chan[v].vol[0] = a->vol_left;
    if (a->volmode_right == VM_DIRECT) chan[v].vol[1] = a->vol_right;
}

int ac3_spu_core_key_status(int v) {
    if (v < 0 || v >= SPU_VOICES) return SPU_ST_OFF;
    int on = ac3_spu_voice[v].keyed_on;
    int env = chan[v].env != 0;
    if (on)  return env ? SPU_ST_ON : SPU_ST_ON_ENV_OFF;
    return env ? SPU_ST_OFF_ENV_ON : SPU_ST_OFF;
}

s16 ac3_spu_core_envx(int v) {
    if (v < 0 || v >= SPU_VOICES) return 0;
    return (s16)chan[v].env;
}

void ac3_spu_core_volx(int v, s16 *l, s16 *r) {
    if (v < 0 || v >= SPU_VOICES) { if (l) *l = 0; if (r) *r = 0; return; }
    if (l) *l = (s16)chan[v].vol[0];
    if (r) *r = (s16)chan[v].vol[1];
}

void ac3_spu_set_master(s16 l, s16 r, s16 ml, s16 mr) {
    master[0] = l; master[1] = r;
    master_mode[0] = ml; master_mode[1] = mr;
    if (ml == VM_DIRECT) master_cur[0] = l;
    if (mr == VM_DIRECT) master_cur[1] = r;
    if (master_cyc[0] < 1) master_cyc[0] = 1;
    if (master_cyc[1] < 1) master_cyc[1] = 1;
}

void ac3_spu_master_volx(s16 *l, s16 *r) {
    if (l) *l = (s16)master_cur[0];
    if (r) *r = (s16)master_cur[1];
}

void ac3_spu_set_mute(int on) { muted = on; }
int  ac3_spu_get_mute(void)   { return muted; }

void ac3_spu_set_cd_volume(s16 l, s16 r) { cd_vol[0] = l; cd_vol[1] = r; }
void ac3_spu_set_cd_reverb(int on)       { cd_reverb_on = on; }
void ac3_spu_set_cd_mix(int on)          { cd_mix_on = on; }
int  ac3_spu_get_cd_mix(void)            { return cd_mix_on; }

void ac3_spu_rvb_set_reg(int i, u16 v) { if (i >= 0 && i < 32) rvb_reg[i] = v; }
u16  ac3_spu_rvb_reg(int i)            { return (i >= 0 && i < 32) ? rvb_reg[i] : 0; }

void ac3_spu_rvb_set_base(u16 v) {
    rvb_mbase = v;
    rvb_wa = ((u32)v << 2) & 0x3FFFFu;
    rvb_cur = rvb_wa;
}
u16 ac3_spu_rvb_base(void) { return rvb_mbase; }

void ac3_spu_rvb_set_volume(s16 l, s16 r) { rvb_vol[0] = l; rvb_vol[1] = r; }
void ac3_spu_rvb_volume(s16 *l, s16 *r) {
    if (l) *l = (s16)rvb_vol[0];
    if (r) *r = (s16)rvb_vol[1];
}

void ac3_spu_rvb_set_enable(int on) { rvb_enable = !!on; }
int  ac3_spu_rvb_enabled(void)      { return rvb_enable; }

void ac3_spu_rvb_run(const s32 in[2], s32 out[2]) {
    s32 rin[2] = { clamp16(in[0]), clamp16(in[1]) };
    rvb_tick(rin, out);
}

void ac3_spu_set_reverb_voices(u32 bits) { reverb_voices = bits & 0xFFFFFFu; }
u32  ac3_spu_get_reverb_voices(void)     { return reverb_voices; }

void ac3_spu_set_noise_voices(u32 bits) { noise_voices = bits; }
u32  ac3_spu_get_noise_voices(void)     { return noise_voices; }
void ac3_spu_set_noise_clock(int c)     { noise_clock = c & 0x3F; }
int  ac3_spu_get_noise_clock(void)      { return noise_clock; }
void ac3_spu_set_pitch_lfo_voices(u32 b) { lfo_voices = b; }
u32  ac3_spu_get_pitch_lfo_voices(void)  { return lfo_voices; }

static const u16 n2p_semitone[12] = {
    32768, 34716, 36780, 38967, 41285, 43740, 46340, 49096, 52015, 55108,
    58385, 61857,
};
static const u16 n2p_fine[128] = {
    32768, 32782, 32797, 32812, 32827, 32842, 32856, 32871, 32886, 32901,
    32916, 32931, 32945, 32960, 32975, 32990, 33005, 33020, 33035, 33050,
    33065, 33080, 33094, 33109, 33124, 33139, 33154, 33169, 33184, 33199,
    33214, 33229, 33244, 33259, 33274, 33289, 33304, 33319, 33334, 33349,
    33364, 33379, 33394, 33410, 33425, 33440, 33455, 33470, 33485, 33500,
    33515, 33530, 33546, 33561, 33576, 33591, 33606, 33621, 33636, 33652,
    33667, 33682, 33697, 33712, 33728, 33743, 33758, 33773, 33789, 33804,
    33819, 33834, 33850, 33865, 33880, 33896, 33911, 33926, 33941, 33957,
    33972, 33987, 34003, 34018, 34033, 34049, 34064, 34080, 34095, 34110,
    34126, 34141, 34157, 34172, 34187, 34203, 34218, 34234, 34249, 34265,
    34280, 34296, 34311, 34327, 34342, 34358, 34373, 34389, 34404, 34420,
    34435, 34451, 34466, 34482, 34497, 34513, 34528, 34544, 34560, 34575,
    34591, 34606, 34622, 34638, 34653, 34669, 34685, 34700,
};

u16 ac3_spu_note_to_pitch(u16 note, u16 sample_note) {
    u32 fine = ((u32)(note & 0xFF) + (u32)(sample_note & 0xFF)) & 0xFFFFu;
    s32 semis = (s16)((note >> 8) + (fine >> 7) - (sample_note >> 8));
    s32 octave = semis / 12;
    s32 shift = octave - 2;
    s32 semi = semis - octave * 12;
    if (semi < 0) { semi += 12; shift = octave - 3; }
    u32 p = ((u32)n2p_semitone[semi] * (u32)n2p_fine[fine & 0x7F]) >> 16;
    if (shift >= 0) return 0x3FFF;
    u32 n = (u32)-shift;
    return (u16)((p + (1u << (n - 1))) >> n);
}

void ac3_spu_cd_push(const s16 *interleaved, int nframes) {
    for (int i = 0; i < nframes; i++) {
        if (cd_wr - cd_rd >= CD_FIFO_FRAMES) {
            cd_rd = cd_wr - (CD_FIFO_FRAMES - 1u);
        }
        u32 s = (cd_wr % CD_FIFO_FRAMES) * 2u;
        cd_fifo[s + 0] = interleaved[i * 2 + 0];
        cd_fifo[s + 1] = interleaved[i * 2 + 1];
        cd_wr++;
    }
}

int ac3_spu_cd_queued(void) { return (int)(cd_wr - cd_rd); }
void ac3_spu_cd_flush(void) { cd_rd = cd_wr; }

static void cd_next(s32 *l, s32 *r) {
    if (cd_rd == cd_wr) { *l = 0; *r = 0; return; }
    u32 s = (cd_rd % CD_FIFO_FRAMES) * 2u;
    *l = cd_fifo[s + 0];
    *r = cd_fifo[s + 1];
    cd_rd++;
}

const s16 *ac3_spu_decoded(int which) {
    if (which < 0 || which > 3) which = 0;
    if (!ac3_spu_ram) return NULL;
    return (const s16 *)(const void *)(ac3_spu_ram + DECODED_BASE(which));
}

u32 ac3_spu_output_peak(void) { return out_peak; }
u64 ac3_spu_frames_mixed(void) { return out_frames; }

void ac3_spu_core_mix(s16 *out, int nframes) {
    for (int f = 0; f < nframes; f++) {
        s32 dry[2] = { 0, 0 };
        s32 wet_in[2] = { 0, 0 };

        s32 noise = noise_voices ? noise_next() : 0;

        s32 prev_out = 0;

        for (int v = 0; v < SPU_VOICES; v++) {
            chan_t *c = &chan[v];
            const spu_voice_t *a = &ac3_spu_voice[v];

            if (c->phase == ENV_OFF) {
                c->out = 0;
                prev_out = 0;
                if (v == 1) decoded_write(2, 0);
                else if (v == 3) decoded_write(3, 0);
                continue;
            }

            int is_noise = (int)((noise_voices >> v) & 1u);
            s32 raw = is_noise ? noise : gauss_interp(c);
            s32 shaped = (raw * c->env) >> 15;
            c->out = shaped;

            s32 l = (shaped * c->vol[0]) >> 14;
            s32 r = (shaped * c->vol[1]) >> 14;
            dry[0] += l;
            dry[1] += r;
            if (reverb_voices & (1u << v)) { wet_in[0] += l; wet_in[1] += r; }

            if (v == 1) decoded_write(2, (s16)clamp16(shaped));
            else if (v == 3) decoded_write(3, (s16)clamp16(shaped));
            if (shaped) voices_played |= (1u << v);

            sweep_tick(&c->vol[0], &c->vol_cyc[0], a->volmode_left, a->vol_left);
            sweep_tick(&c->vol[1], &c->vol_cyc[1], a->volmode_right, a->vol_right);

            env_tick(c, a);
            if (!is_noise) {
                u32 step = a->pitch;
                if ((lfo_voices & (1u << v)) && v > 0)
                    step = (u32)((s32)step + (((s32)(s16)step * prev_out) >> 15));
                if (step > 0x3FFFu) step = 0x3FFFu;
                c->counter += step;
                while (c->counter >= 0x1000u) {
                    c->counter -= 0x1000u;
                    if (!voice_pull(c, v)) break;
                }
            }
            prev_out = shaped;
        }

        if (muted) {
            dry[0] = dry[1] = 0;
            wet_in[0] = wet_in[1] = 0;
        }

        s32 cl = 0, cr = 0;
        cd_next(&cl, &cr);
        decoded_write(0, (s16)clamp16(cl));
        decoded_write(1, (s16)clamp16(cr));
        if (cd_mix_on) {
            s32 gl = (cl * cd_vol[0]) >> 15;
            s32 gr = (cr * cd_vol[1]) >> 15;
            dry[0] += gl;
            dry[1] += gr;
            if (cd_reverb_on) { wet_in[0] += gl; wet_in[1] += gr; }
        }

        {
            s32 rin[2] = { clamp16(wet_in[0]), clamp16(wet_in[1]) };
            s32 rout[2];
            rvb_tick(rin, rout);
            for (int ch = 0; ch < 2; ch++)
                dry[ch] = clamp16(dry[ch] + ((rout[ch] * rvb_vol[ch]) >> 15));
        }

        sweep_tick(&master_cur[0], &master_cyc[0], master_mode[0], master[0]);
        sweep_tick(&master_cur[1], &master_cyc[1], master_mode[1], master[1]);

        s32 fl = clamp16((dry[0] * master_cur[0]) >> 14);
        s32 fr = clamp16((dry[1] * master_cur[1]) >> 14);
        out[f * 2 + 0] = (s16)fl;
        out[f * 2 + 1] = (s16)fr;

        u32 mag = (u32)(fl < 0 ? -fl : fl);
        if (mag > out_peak) out_peak = mag;
        mag = (u32)(fr < 0 ? -fr : fr);
        if (mag > out_peak) out_peak = mag;

        decoded_pos = (decoded_pos + 1u) % SPU_DECODED_SIZE;
        out_frames++;
    }
}

static FILE *dump_fp;
static u32 dump_frames;

static void put32(FILE *f, u32 v) {
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void put16(FILE *f, u16 v) {
    u8 b[2] = { (u8)v, (u8)(v >> 8) };
    fwrite(b, 1, 2, f);
}

int ac3_spu_dump_open(const char *path) {
    if (dump_fp) ac3_spu_dump_close();
    dump_fp = fopen(path, "wb");
    if (!dump_fp) {
        fprintf(stderr, "ac3: cannot open %s for audio dump\n", path);
        return 0;
    }
    dump_frames = 0;
    fwrite("RIFF", 1, 4, dump_fp);
    put32(dump_fp, 0);
    fwrite("WAVEfmt ", 1, 8, dump_fp);
    put32(dump_fp, 16);
    put16(dump_fp, 1);
    put16(dump_fp, 2);
    put32(dump_fp, SPU_SAMPLE_RATE);
    put32(dump_fp, SPU_SAMPLE_RATE * 4);
    put16(dump_fp, 4);
    put16(dump_fp, 16);
    fwrite("data", 1, 4, dump_fp);
    put32(dump_fp, 0);
    printf("audio: writing the mix to %s\n", path);
    return 1;
}

void ac3_spu_dump_write(const s16 *frames, int nframes) {
    if (!dump_fp || nframes <= 0) return;
    fwrite(frames, sizeof(s16) * 2, (size_t)nframes, dump_fp);
    dump_frames += (u32)nframes;
}

void ac3_spu_dump_close(void) {
    if (!dump_fp) return;
    u32 data = dump_frames * 4u;
    fseek(dump_fp, 4, SEEK_SET);
    put32(dump_fp, 36u + data);
    fseek(dump_fp, 40, SEEK_SET);
    put32(dump_fp, data);
    fclose(dump_fp);
    dump_fp = NULL;
    printf("audio: wrote %u frames (%.1f s)\n", dump_frames,
           (double)dump_frames / (double)SPU_SAMPLE_RATE);
}

extern void ac3_audio_queue_stats(u32 *under, u32 *over);

void ac3_spu_stats_dump(void) {
    if (!out_frames) { printf("\nSPU: nothing mixed\n"); return; }
    printf("\nSPU: %llu frames mixed (%.1f s), output peak %u/32767\n",
           (unsigned long long)out_frames,
           (double)out_frames / (double)SPU_SAMPLE_RATE, out_peak);
    printf("  voices that produced audio: ");
    int n = 0;
    for (int v = 0; v < SPU_VOICES; v++)
        if (voices_played & (1u << v)) { printf("%d ", v); n++; }
    printf("%s(%d of %d)\n", n ? "" : "none ", n, SPU_VOICES);
    printf("  reverb writes %s, output %d,%d, work area 0x%05X-0x7FFFF, "
           "voices 0x%06X, CD mix %s\n",
           rvb_enable ? "on" : "off", (int)rvb_vol[0], (int)rvb_vol[1],
           (unsigned)rvb_mbase * 8u, reverb_voices, cd_mix_on ? "on" : "off");
    {
        u32 under = 0, over = 0;
        ac3_audio_queue_stats(&under, &over);
        u32 fields = (u32)(out_frames * SPU_FIELD_DEN / SPU_FIELD_NUM);
        if (under || over) {
            printf("  device queue: %u underruns, %u overruns in %u fields\n",
                   under, over, fields);
            if (fields && (under + over) * 100u / fields >= 2u)
                printf("        that is %.1f%% of fields -- the emulation is "
                       "not tracking real time, and the audio will stutter\n",
                       100.0 * (double)(under + over) / (double)fields);
        }
    }
}

void ac3_spu_core_machine_save(FILE *f) {
    ac3_ms_tag(f, "SPUC");
    for (int v = 0; v < SPU_VOICES; v++) {
        const chan_t *c = &chan[v];
        ac3_ms_u32(f, c->cur);
        ac3_ms_u32(f, (u32)c->have_block);
        ac3_ms_u32(f, c->flags);
        for (int i = 0; i < 28; i++) ac3_ms_u32(f, (u32)(s32)c->dec[i]);
        for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)c->hist[i]);
        ac3_ms_u32(f, (u32)c->idx);
        ac3_ms_u32(f, c->counter);
        for (int i = 0; i < 4; i++) ac3_ms_u32(f, (u32)(s32)c->win[i]);
        ac3_ms_u32(f, (u32)c->ended);
        ac3_ms_u32(f, (u32)c->phase);
        ac3_ms_u32(f, (u32)c->env);
        ac3_ms_u32(f, c->env_div);
        ac3_ms_u32(f, (u32)c->out);
        for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)c->vol[i]);
        for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)c->vol_cyc[i]);
        ac3_ms_u32(f, repeat_addr[v]);
        ac3_ms_u32(f, ignore_loop_start[v]);
    }
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)master[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)master_mode[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)master_cur[i]);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)master_cyc[i]);
    ac3_ms_u32(f, (u32)muted);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)cd_vol[i]);
    ac3_ms_u32(f, (u32)cd_reverb_on);
    ac3_ms_u32(f, (u32)cd_mix_on);
    ac3_ms_u32(f, cd_rd);
    ac3_ms_u32(f, cd_wr);
    ac3_ms_u32(f, (cd_wr - cd_rd) * 4u);
    for (u32 k = cd_rd; k != cd_wr; k++) {
        u32 s = (k % CD_FIFO_FRAMES) * 2u;
        fwrite(&cd_fifo[s], 2, 2, f);
    }
    ac3_ms_u32(f, noise_voices);
    ac3_ms_u32(f, reverb_voices);
    ac3_ms_u32(f, lfo_voices);
    ac3_ms_u32(f, (u32)noise_clock);
    ac3_ms_u32(f, noise_lfsr);
    ac3_ms_u32(f, (u32)noise_timer);
    ac3_ms_u32(f, decoded_pos);
    ac3_ms_u32(f, field_acc);
    for (int i = 0; i < 32; i++) ac3_ms_u32(f, rvb_reg[i]);
    ac3_ms_u32(f, rvb_mbase);
    ac3_ms_u32(f, rvb_wa);
    ac3_ms_u32(f, rvb_cur);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)rvb_vol[i]);
    ac3_ms_u32(f, (u32)rvb_enable);
    ac3_ms_bytes(f, rvb_down, (u32)sizeof rvb_down);
    ac3_ms_bytes(f, rvb_up, (u32)sizeof rvb_up);
    ac3_ms_u32(f, rvb_pos);
}
