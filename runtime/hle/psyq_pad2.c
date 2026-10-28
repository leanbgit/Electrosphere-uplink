#include "ac3_input.h"
#include <string.h>
#include "ac3_oracle.h"

#define PadStateDiscon    0
#define PadStateFindPad   1
#define PadStateStable    6

#define InfoModeCurID     1
#define InfoModeCurExID   2
#define InfoModeCurExOffs 3
#define InfoModeIdTable   4

static u32 direct_buf[2];
static int com_enabled = 1;

void ac3_pad_fill_buffer(int port, u32 buf);

void psyq_PadInitDirect(ctx_t *ctx) {
    direct_buf[0] = A0;
    direct_buf[1] = A1;
    hle_trace("PadInitDirect", "buf0=0x%08X buf1=0x%08X", A0, A1);
    RET(0);
}

void psyq_PadInit(ctx_t *ctx) { RET(0); }
void psyq_PadStop(ctx_t *ctx) { RET(0); }

void psyq_PadGetState(ctx_t *ctx) {
    int port = (A0 & 0x10) ? 1 : 0;
    RET(ac3_pad[port].connected ? PadStateStable : PadStateDiscon);
}

void psyq_PadInfoMode(ctx_t *ctx) {
    int port = (A0 & 0x10) ? 1 : 0;
    u32 term = A1;
    const ac3_pad_t *p = &ac3_pad[port];
    if (!p->connected) { RET(0); return; }

    switch (term) {
    case InfoModeCurID:
        RET(p->type >> 4);
        return;
    case InfoModeCurExID:
        RET(p->type == AC3_PAD_ANALOG ? 7 : 0);
        return;
    case InfoModeCurExOffs:
        RET(0);
        return;
    case InfoModeIdTable:
        RET(A2 == 0 ? (u32)(p->type >> 4) : 0);
        return;
    default:
        RET(0);
        return;
    }
}

void psyq_PadInfoAct(ctx_t *ctx) {
    int port = (A0 & 0x10) ? 1 : 0;
    RET(ac3_pad[port].type == AC3_PAD_ANALOG ? 2 : 0);
}

void psyq_PadInfoComb(ctx_t *ctx) { RET(0); }

void psyq_PadEnableCom(ctx_t *ctx) {
    u32 prev = (u32)com_enabled;
    com_enabled = (int)A0;
    RET(prev);
}

void psyq_PadEnableGun(ctx_t *ctx) { RET(0); }

void psyq_PadChkVsync(ctx_t *ctx) { RET(1); }

void psyq_PadChkMtap(ctx_t *ctx) { RET(0); }

void psyq_PadSetAct(ctx_t *ctx) {
    int port = (A0 & 0x10) ? 1 : 0;
    u32 tbl = A1, len = A2;
    if (tbl && len >= 1) ac3_pad[port].rumble_small = (u8)MEM_R8U(tbl);
    if (tbl && len >= 2) ac3_pad[port].rumble_large = (u8)MEM_R8U(tbl + 1);
    RET(1);
}

void psyq_PadSetActAlign(ctx_t *ctx) { RET(1); }
void psyq_PadSetMainMode(ctx_t *ctx) { RET(1); }

void psyq_EnablePAD(ctx_t *ctx)  { com_enabled = 1; RET(0); }
void psyq_DisablePAD(ctx_t *ctx) { com_enabled = 0; RET(0); }

#define LIBPAD_PORT_ARRAY_PTR 0x800B46B8u
#define LIBPAD_PORT_STRIDE    240u
#define LIBPAD_BUF_OFFSET     48u

static u32 direct_buffer(int port);
#ifdef AC3_COOP_PROBE
u32 ac3_coop_pad_buffer(int port) { return direct_buffer(port); }
#endif
static u32 direct_buffer(int port) {
    if (direct_buf[port]) return direct_buf[port];
    u32 ports = MEM_R32(LIBPAD_PORT_ARRAY_PTR);
    if (!ports) return 0;
    u32 p = MEM_R32(ports + LIBPAD_BUF_OFFSET + (u32)port * LIBPAD_PORT_STRIDE);
    return (p >= 0x80000000u && p < 0x80200000u) ? p : 0;
}

void ac3_pad2_update(void) {
    for (int port = 0; port < 2; port++) {
        u32 buf = direct_buffer(port);
        if (buf) ac3_pad_fill_buffer(port, buf);
    }
}

void ac3_pad2_machine_save(FILE *f) {
    ac3_ms_tag(f, "PAD2");
    ac3_ms_u32(f, direct_buf[0]);
    ac3_ms_u32(f, direct_buf[1]);
    ac3_ms_u32(f, (u32)com_enabled);
}
