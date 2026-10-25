#include "hle.h"
#include <stdlib.h>
#include "ac3_oracle.h"

#define MAX_EVENTS 32
#define EV_DESC_BASE 0xF1000000u

#define EvMdINTR   0x1000u
#define EvMdNOINTR 0x2000u

#define EvStUNUSED  0x0000u
#define EvStWAIT    0x1000u
#define EvStACTIVE  0x2000u
#define EvStALREADY 0x4000u

typedef struct {
    int used;
    u32 cls, spec, mode, func;
    u32 state;
} event_t;

static event_t events[MAX_EVENTS];

static int event_trace(void) {
    static int on = -1;
    if (on < 0) { const char *s = getenv("AC3_EVENT_TRACE"); on = s && *s != '0'; }
    return on;
}

static event_t *event_from_desc(u32 desc) {
    u32 i = desc - EV_DESC_BASE;
    return (i < MAX_EVENTS && events[i].used) ? &events[i] : NULL;
}

void psyq_OpenEvent(ctx_t *ctx) {
    for (u32 i = 0; i < MAX_EVENTS; i++) {
        if (events[i].used) continue;
        events[i].used = 1;
        events[i].cls = A0;
        events[i].spec = A1;
        events[i].mode = A2;
        events[i].func = A3;
        events[i].state = EvStWAIT;
        hle_trace("OpenEvent", "class=0x%08X spec=0x%X mode=0x%X fn=0x%08X",
                  A0, A1, A2, A3);
        RET(EV_DESC_BASE + i);
        return;
    }
    hle_warn("OpenEvent", "event table full (%d)", MAX_EVENTS);
    RET(0xFFFFFFFFu);
}

void psyq_CloseEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    if (e) { e->used = 0; e->state = EvStUNUSED; RET(1); } else RET(0);
}

void psyq_EnableEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    if (event_trace())
        hle_trace("EnableEvent", "desc 0x%08X%s", A0, e ? "" : " (unknown)");
    if (e) { e->state = EvStACTIVE; RET(1); } else RET(0);
}

void psyq_DisableEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    if (e) { e->state = EvStWAIT; RET(1); } else RET(0);
}

void ac3_deliver_event(ctx_t *ctx, u32 cls, u32 spec) {
    int hit = 0;
    for (u32 i = 0; i < MAX_EVENTS; i++) {
        event_t *e = &events[i];
        if (!e->used || e->state != EvStACTIVE) continue;
        if (e->cls != cls || !(e->spec & spec)) continue;
        hit++;
        if (e->mode == EvMdINTR && e->func)
            ac3_call_indirect(ctx, e->func);
        else
            e->state = EvStALREADY;
    }
    if (event_trace())
        hle_trace("deliver", "class 0x%08X spec 0x%04X -> %d listener(s)",
                  cls, spec, hit);
}

void psyq_DeliverEvent(ctx_t *ctx) {
    ac3_deliver_event(ctx, A0, A1);
    RET(0);
}

void psyq_TestEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    int fired = e && e->state == EvStALREADY;
    if (event_trace() && e)
        hle_trace("TestEvent", "desc 0x%08X class 0x%08X spec 0x%04X -> %d",
                  A0, e->cls, e->spec, fired);
    if (fired) {
        e->state = EvStACTIVE;
        RET(1);
    } else {
        RET(0);
    }
}

void psyq_WaitEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    if (e && e->state == EvStALREADY) {
        e->state = EvStACTIVE;
        RET(1);
        return;
    }
    RET(1);
}

void psyq_UnDeliverEvent(ctx_t *ctx) {
    event_t *e = event_from_desc(A0);
    if (e && e->state == EvStALREADY) e->state = EvStACTIVE;
    RET(0);
}

typedef struct { u32 target, mode; int running; } rcnt_t;
static rcnt_t rcnt[4];

extern u32 ac3_vblank_count(void);
extern u32 ac3_hblank_count(void);

static u32 rcnt_index(u32 spec) { return spec & 3u; }

void psyq_SetRCnt(ctx_t *ctx) {
    u32 i = rcnt_index(A0);
    rcnt[i].target = A1;
    rcnt[i].mode = A2;
    RET(1);
}

void psyq_GetRCnt(ctx_t *ctx) {
    u32 i = rcnt_index(A0);
    switch (i) {
    case 1: RET(ac3_hblank_count() & 0xFFFFu); return;
    case 3: RET(ac3_vblank_count() & 0xFFFFu); return;
    default: RET((ac3_hblank_count() * 8u) & 0xFFFFu); return;
    }
}

void psyq_StartRCnt(ctx_t *ctx) { rcnt[rcnt_index(A0)].running = 1; RET(1); }
void psyq_StopRCnt(ctx_t *ctx)  { rcnt[rcnt_index(A0)].running = 0; RET(1); }
void psyq_ResetRCnt(ctx_t *ctx) { RET(1); }
void psyq_ChangeClearRCnt(ctx_t *ctx) { RET(0); }

static int critical_depth;

void psyq_EnterCriticalSection(ctx_t *ctx) {
    int was = critical_depth;
    critical_depth++;
    RET(was == 0);
}

void psyq_ExitCriticalSection(ctx_t *ctx) {
    if (critical_depth > 0) critical_depth--;
    RET(0);
}

void psyq_FlushCache(ctx_t *ctx) { RET(0); }

void psyq_InitHeap(ctx_t *ctx) {
    hle_trace("InitHeap", "addr=0x%08X size=%u", A0, A1);
    RET(0);
}

void psyq_GPU_cw(ctx_t *ctx) {
    u32 cmd = A0 >> 24;
    if (cmd < 0xE0) hle_warn("GPU_cw", "unexpected raw command 0x%02X", cmd);
    RET(0);
}

void psyq_ReturnFromException(ctx_t *ctx) { RET(0); }
void psyq_ResetEntryInt(ctx_t *ctx) { RET(0); }
void psyq_HookEntryInt(ctx_t *ctx) { RET(0); }
void psyq_SysEnqIntRP(ctx_t *ctx) { RET(0); }
void psyq_SysDeqIntRP(ctx_t *ctx) { RET(0); }

void psyq_Krom2RawAdd(ctx_t *ctx) { RET(0xFFFFFFFFu); }

void psyq__patch_gte(ctx_t *ctx)  { RET(0); }
void psyq__patch_pad(ctx_t *ctx)  { RET(0); }
void psyq__patch_card(ctx_t *ctx) { RET(0); }
void psyq__patch_card2(ctx_t *ctx) { RET(0); }
void psyq__patch_card_info(ctx_t *ctx) { RET(0); }
void psyq__copy_memcard_patch(ctx_t *ctx) { RET(0); }
void psyq__96_remove(ctx_t *ctx) { RET(0); }
void psyq__bu_init(ctx_t *ctx) { RET(0); }

void ac3_api_machine_save(FILE *f) {
    ac3_ms_tag(f, "API");
    for (int i = 0; i < MAX_EVENTS; i++) {
        ac3_ms_u32(f, (u32)events[i].used);
        ac3_ms_u32(f, events[i].cls);
        ac3_ms_u32(f, events[i].spec);
        ac3_ms_u32(f, events[i].mode);
        ac3_ms_u32(f, events[i].func);
        ac3_ms_u32(f, events[i].state);
    }
    for (int i = 0; i < 4; i++) {
        ac3_ms_u32(f, rcnt[i].target);
        ac3_ms_u32(f, rcnt[i].mode);
        ac3_ms_u32(f, (u32)rcnt[i].running);
    }
    ac3_ms_u32(f, (u32)critical_depth);
}
