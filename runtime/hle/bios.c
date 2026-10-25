#include "hle.h"

extern void psyq_OpenEvent(ctx_t *), psyq_CloseEvent(ctx_t *);
extern void psyq_WaitEvent(ctx_t *), psyq_TestEvent(ctx_t *);
extern void psyq_EnableEvent(ctx_t *), psyq_DisableEvent(ctx_t *);
extern void psyq_DeliverEvent(ctx_t *), psyq_UnDeliverEvent(ctx_t *);
extern void psyq_ReturnFromException(ctx_t *);
extern void psyq_ResetEntryInt(ctx_t *), psyq_HookEntryInt(ctx_t *);
extern void psyq_SysEnqIntRP(ctx_t *), psyq_SysDeqIntRP(ctx_t *);
extern void psyq_ChangeClearRCnt(ctx_t *);
extern void psyq_InitHeap(ctx_t *), psyq_FlushCache(ctx_t *);
extern void psyq_GPU_cw(ctx_t *), psyq_Krom2RawAdd(ctx_t *);
extern void psyq__96_remove(ctx_t *), psyq__bu_init(ctx_t *);

extern void psyq_open(ctx_t *), psyq_lseek(ctx_t *), psyq_read(ctx_t *);
extern void psyq_write(ctx_t *), psyq_close(ctx_t *);
extern void psyq_firstfile(ctx_t *), psyq_nextfile(ctx_t *);
extern void psyq_InitCARD(ctx_t *), psyq_StartCARD(ctx_t *), psyq_StopCARD(ctx_t *);
extern void psyq__card_info(ctx_t *), psyq__card_load(ctx_t *);
extern void psyq__card_read(ctx_t *), psyq__card_write(ctx_t *);
extern void psyq__card_status(ctx_t *), psyq__new_card(ctx_t *);

static void bios_memcpy(ctx_t *ctx) {
    u32 d = A0, s = A1, n = A2;
    for (u32 i = 0; i < n; i++) MEM_W8(d + i, MEM_R8U(s + i));
    RET(d);
}

static void bios_memset(ctx_t *ctx) {
    u32 d = A0, c = A1, n = A2;
    for (u32 i = 0; i < n; i++) MEM_W8(d + i, c);
    RET(d);
}

static void bios_strlen(ctx_t *ctx) {
    u32 p = A0, n = 0;
    while (MEM_R8U(p + n)) n++;
    RET(n);
}

static void bios_strcmp(ctx_t *ctx) {
    u32 a = A0, b = A1;
    for (;;) {
        u32 ca = MEM_R8U(a), cb = MEM_R8U(b);
        if (ca != cb) { RET((s32)ca - (s32)cb); return; }
        if (!ca) { RET(0); return; }
        a++; b++;
    }
}

static void bios_strcpy(ctx_t *ctx) {
    u32 d = A0, s = A1;
    for (u32 i = 0;; i++) {
        u32 c = MEM_R8U(s + i);
        MEM_W8(d + i, c);
        if (!c) break;
    }
    RET(d);
}

static void bios_rand(ctx_t *ctx) {
    static u32 seed = 1;
    seed = seed * 1103515245u + 12345u;
    RET((seed >> 16) & 0x7FFFu);
}

static void bios_srand(ctx_t *ctx) { RET(0); }

static void unhandled(ctx_t *ctx, u32 vector, u32 fn) {
    hle_warn("bios", "unhandled vector 0x%02X function 0x%02X", vector, fn);
    RET(0);
}

void ac3_bios_call(ctx_t *ctx, u32 vector, u32 funcnum) {
    u32 fn = funcnum & 0xFF;
    switch (vector & 0xFF) {
    case 0xA0:
        switch (fn) {
        case 0x2A: bios_memcpy(ctx); return;
        case 0x2B: bios_memset(ctx); return;
        case 0x2F: bios_rand(ctx); return;
        case 0x30: bios_srand(ctx); return;
        case 0x17: bios_strcmp(ctx); return;
        case 0x19: bios_strcpy(ctx); return;
        case 0x1B: bios_strlen(ctx); return;
        case 0x39: psyq_InitHeap(ctx); return;
        case 0x44: psyq_FlushCache(ctx); return;
        case 0x49: psyq_GPU_cw(ctx); return;
        case 0x70: psyq__bu_init(ctx); return;
        case 0x72: psyq__96_remove(ctx); return;
        case 0xAB: psyq__card_info(ctx); return;
        case 0xAC: psyq__card_load(ctx); return;
        default: unhandled(ctx, vector, fn); return;
        }
    case 0xB0:
        switch (fn) {
        case 0x07: psyq_DeliverEvent(ctx); return;
        case 0x08: psyq_OpenEvent(ctx); return;
        case 0x09: psyq_CloseEvent(ctx); return;
        case 0x0A: psyq_WaitEvent(ctx); return;
        case 0x0B: psyq_TestEvent(ctx); return;
        case 0x0C: psyq_EnableEvent(ctx); return;
        case 0x0D: psyq_DisableEvent(ctx); return;
        case 0x17: psyq_ReturnFromException(ctx); return;
        case 0x18: psyq_ResetEntryInt(ctx); return;
        case 0x19: psyq_HookEntryInt(ctx); return;
        case 0x20: psyq_UnDeliverEvent(ctx); return;
        case 0x32: psyq_open(ctx); return;
        case 0x33: psyq_lseek(ctx); return;
        case 0x34: psyq_read(ctx); return;
        case 0x35: psyq_write(ctx); return;
        case 0x36: psyq_close(ctx); return;
        case 0x3D: RET(0); return;
        case 0x3E: fputc((int)A0, stderr); RET(A0); return;
        case 0x42: psyq_firstfile(ctx); return;
        case 0x43: psyq_nextfile(ctx); return;
        case 0x4A: psyq_InitCARD(ctx); return;
        case 0x4B: psyq_StartCARD(ctx); return;
        case 0x4C: psyq_StopCARD(ctx); return;
        case 0x4D: psyq__card_info(ctx); return;
        case 0x4E: psyq__card_write(ctx); return;
        case 0x4F: psyq__card_read(ctx); return;
        case 0x50: psyq__new_card(ctx); return;
        case 0x51: psyq_Krom2RawAdd(ctx); return;
        case 0x5C: psyq__card_status(ctx); return;
        default: unhandled(ctx, vector, fn); return;
        }
    case 0xC0:
        switch (fn) {
        case 0x02: psyq_SysEnqIntRP(ctx); return;
        case 0x03: psyq_SysDeqIntRP(ctx); return;
        case 0x0A: psyq_ChangeClearRCnt(ctx); return;
        default: unhandled(ctx, vector, fn); return;
        }
    default:
        hle_warn("bios", "call to non-vector 0x%08X", vector);
        RET(0);
        return;
    }
}

extern void psyq_EnterCriticalSection(ctx_t *);
extern void psyq_ExitCriticalSection(ctx_t *);

void ac3_syscall(ctx_t *ctx) {
    switch (A0) {
    case 0: RET(0); return;
    case 1: psyq_EnterCriticalSection(ctx); return;
    case 2: psyq_ExitCriticalSection(ctx); return;
    default:
        hle_warn("syscall", "unknown function %u", A0);
        RET(0);
        return;
    }
}

void ac3_break(ctx_t *ctx) {
    hle_warn("break", "BREAK executed at pc 0x%08X", ctx->pc);
}
