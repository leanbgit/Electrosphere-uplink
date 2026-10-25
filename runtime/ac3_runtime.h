#ifndef AC3_RUNTIME_H
#define AC3_RUNTIME_H

#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef struct {
    s16 v[3][3];
    u32 rgbc;
    u16 otz;
    s16 ir[4];
    s16 sxy[4][2];
    u16 sz[4];
    u32 rgb[3];
    u32 res1;
    s32 mac[4];
    u32 irgb, orgb;
    s32 lzcs; s32 lzcr;
    s16 rt[3][3]; s32 tr[3];
    s16 llm[3][3]; s32 bk[3];
    s16 lcm[3][3]; s32 fc[3];
    s32 ofx, ofy; u16 h; s16 dqa; s32 dqb;
    s16 zsf3, zsf4;
    u32 flag;
} gte_t;

typedef struct {
    u32 r[32];
    u32 hi, lo;
    u32 pc;
    gte_t gte;
    u32 cop0[16];
} ctx_t;

#define AC3_RAM_SIZE   0x200000u
#define AC3_RAM_MASK   0x1FFFFFu

extern u8 *ac3_ram;
extern u8 *ac3_scratch;

u32  ac3_mem_read_slow(u32 addr, int size);
void ac3_mem_write_slow(u32 addr, u32 value, int size);

static inline u32 ac3_phys(u32 a) { return a & 0x1FFFFFFFu; }

static inline u32 MEM_R32(u32 a) {
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) { u32 v; memcpy(&v, ac3_ram + p, 4); return v; }
    return ac3_mem_read_slow(a, 4);
}
static inline u32 MEM_R16U(u32 a) {
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) { u16 v; memcpy(&v, ac3_ram + p, 2); return v; }
    return ac3_mem_read_slow(a, 2);
}
static inline u32 MEM_R8U(u32 a) {
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) return ac3_ram[p];
    return ac3_mem_read_slow(a, 1);
}
static inline s32 MEM_R16S(u32 a) { return (s32)(s16)MEM_R16U(a); }
static inline s32 MEM_R8S(u32 a)  { return (s32)(s8)MEM_R8U(a); }

extern u32 ac3_watch_lo, ac3_watch_hi;
void ac3_watch_hit(u32 addr, u32 value, int size);

#define AC3_WATCH(a, v, n)     do { if (ac3_watch_hi && (a) >= ac3_watch_lo && (a) < ac3_watch_hi)              ac3_watch_hit((a), (v), (n)); } while (0)

static inline void MEM_W32(u32 a, u32 v) {
    AC3_WATCH(a, v, 4);
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) { memcpy(ac3_ram + p, &v, 4); return; }
    ac3_mem_write_slow(a, v, 4);
}
static inline void MEM_W16(u32 a, u32 v) {
    AC3_WATCH(a, v, 2);
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) { u16 h = (u16)v; memcpy(ac3_ram + p, &h, 2); return; }
    ac3_mem_write_slow(a, v, 2);
}
static inline void MEM_W8(u32 a, u32 v) {
    AC3_WATCH(a, v, 1);
    u32 p = ac3_phys(a);
    if (p < AC3_RAM_SIZE) { ac3_ram[p] = (u8)v; return; }
    ac3_mem_write_slow(a, v, 1);
}

static inline u32 MEM_LWL(u32 old, u32 a) {
    u32 al = a & 3u, w = MEM_R32(a & ~3u);
    u32 sh = (3u - al) * 8u;
    u32 mask = (al == 3u) ? 0u : (0xFFFFFFFFu >> ((al + 1u) * 8u));
    return (old & mask) | (w << sh);
}
static inline u32 MEM_LWR(u32 old, u32 a) {
    u32 al = a & 3u, w = MEM_R32(a & ~3u);
    u32 sh = al * 8u;
    u32 mask = (al == 0u) ? 0u : (0xFFFFFFFFu << ((4u - al) * 8u));
    return (old & mask) | (w >> sh);
}
static inline void MEM_SWL(u32 a, u32 v) {
    u32 al = a & 3u, base = a & ~3u, w = MEM_R32(base);
    u32 sh = (3u - al) * 8u;
    u32 mask = (al == 3u) ? 0u : (0xFFFFFFFFu << ((al + 1u) * 8u));
    MEM_W32(base, (w & mask) | (v >> sh));
}
static inline void MEM_SWR(u32 a, u32 v) {
    u32 al = a & 3u, base = a & ~3u, w = MEM_R32(base);
    u32 sh = al * 8u;
    u32 mask = (al == 0u) ? 0u : (0xFFFFFFFFu >> ((4u - al) * 8u));
    MEM_W32(base, (w & mask) | (v << sh));
}

static inline u32 SHL(u32 v, u32 s)  { return v << (s & 31u); }
static inline u32 SHR(u32 v, u32 s)  { return v >> (s & 31u); }
static inline u32 SAR(u32 v, u32 s)  { return (u32)((s32)v >> (s & 31u)); }
static inline u32 SLT(u32 a, u32 b)  { return (s32)a < (s32)b; }
static inline u32 SLTU(u32 a, u32 b) { return a < b; }

void ac3_mult(ctx_t *ctx, u32 a, u32 b);
void ac3_multu(ctx_t *ctx, u32 a, u32 b);
void ac3_div(ctx_t *ctx, u32 a, u32 b);
void ac3_divu(ctx_t *ctx, u32 a, u32 b);

typedef void (*ac3_func_t)(ctx_t *);

void ac3_call_indirect(ctx_t *ctx, u32 target);
void ac3_jump_indirect(ctx_t *ctx, u32 target);
ac3_func_t ac3_lookup(u32 addr);

void ac3_bios_call(ctx_t *ctx, u32 vector, u32 funcnum);
void ac3_syscall(ctx_t *ctx);
void ac3_break(ctx_t *ctx);

void ac3_mtc0(ctx_t *ctx, u32 reg, u32 value);
u32  ac3_mfc0(ctx_t *ctx, u32 reg);
void ac3_rfe(ctx_t *ctx);

void ac3_gte_op(ctx_t *ctx, u32 opcode);
void ac3_gte_mtc2(ctx_t *ctx, u32 reg, u32 value);
u32  ac3_gte_mfc2(ctx_t *ctx, u32 reg);
void ac3_gte_ctc2(ctx_t *ctx, u32 reg, u32 value);
u32  ac3_gte_cfc2(ctx_t *ctx, u32 reg);

void ac3_unimplemented(ctx_t *ctx, u32 addr, const char *what);

#include <setjmp.h>
void ac3_interpret(ctx_t *ctx, u32 entry);
extern u32 ac3_interp_step_budget;
extern int ac3_interp_budget_hit;

int ac3_setjmp_save(ctx_t *ctx, u32 buf);
jmp_buf *ac3_setjmp_env(int slot);

#endif
