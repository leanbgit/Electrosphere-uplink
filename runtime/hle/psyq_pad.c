#include "ac3_input.h"
#include <string.h>
#include "ac3_oracle.h"

ac3_pad_t ac3_pad[2];

static u32 pad_buf[2];
static u32 pad_len[2];
static int pad_started;

static void ensure_defaults(void) {
    for (int port = 0; port < 2; port++) {
        if (ac3_pad[port].type != 0) continue;
        ac3_pad[port].type = AC3_PAD_DIGITAL;
        ac3_pad[port].connected = (port == 0);
        ac3_pad[port].lx = ac3_pad[port].ly = 0x80;
        ac3_pad[port].rx = ac3_pad[port].ry = 0x80;
    }
}

void ac3_pad_fill_buffer(int port, u32 buf) {
    if (!buf) return;
    const ac3_pad_t *p = &ac3_pad[port];

    if (!p->connected) {
        MEM_W8(buf + 0, 0xFF);
        MEM_W8(buf + 1, 0x00);
        MEM_W8(buf + 2, 0xFF);
        MEM_W8(buf + 3, 0xFF);
        return;
    }
    u16 inv = (u16)~p->buttons;
    MEM_W8(buf + 0, 0x00);
    MEM_W8(buf + 1, p->type);
    MEM_W8(buf + 2, (u8)(inv & 0xFF));
    MEM_W8(buf + 3, (u8)(inv >> 8));
    if (p->type == AC3_PAD_ANALOG &&
        (pad_len[port] == 0 || pad_len[port] >= 8)) {
        MEM_W8(buf + 4, p->rx);
        MEM_W8(buf + 5, p->ry);
        MEM_W8(buf + 6, p->lx);
        MEM_W8(buf + 7, p->ly);
    }
}

extern void ac3_pad2_update(void);

void ac3_pad_update(ctx_t *ctx) {
    (void)ctx;
    ensure_defaults();
    ac3_pad_fill_buffer(0, pad_buf[0]);
    ac3_pad_fill_buffer(1, pad_buf[1]);
    ac3_pad2_update();
}

void psyq_InitPAD2(ctx_t *ctx) {
    pad_buf[0] = A0; pad_len[0] = A1;
    pad_buf[1] = A2; pad_len[1] = A3;
    ensure_defaults();
    hle_trace("InitPAD", "buf0=0x%08X len0=%u buf1=0x%08X len1=%u",
              A0, A1, A2, A3);
    RET(0);
}

void psyq_PAD_init2(ctx_t *ctx) {
    pad_buf[0] = A1; pad_len[0] = A2;
    pad_buf[1] = A3; pad_len[1] = 34;
    ensure_defaults();
    RET(1);
}

void psyq_StartPAD2(ctx_t *ctx) {
    pad_started = 1;
    ensure_defaults();
    ac3_pad_fill_buffer(0, pad_buf[0]);
    ac3_pad_fill_buffer(1, pad_buf[1]);
    RET(0);
}

void psyq_StopPAD2(ctx_t *ctx) { pad_started = 0; RET(0); }

void psyq_ChangeClearPAD(ctx_t *ctx) { RET(0); }
void psyq__remove_ChgclrPAD(ctx_t *ctx) { RET(0); }

void psyq_PadStartCom(ctx_t *ctx) { RET(0); }
void psyq_PadStopCom(ctx_t *ctx) { RET(0); }
void psyq__padIsVsync(ctx_t *ctx) { RET(1); }
void psyq__padIntPad(ctx_t *ctx) { RET(0); }
void psyq__padInitSioMode(ctx_t *ctx) { RET(0); }
void psyq__padClrIntSio0(ctx_t *ctx) { RET(0); }
void psyq__padWaitRXready(ctx_t *ctx) { RET(1); }
void psyq__padSetCmd(ctx_t *ctx) { RET(0); }
void psyq__padCmdParaMode(ctx_t *ctx) { RET(0); }
void psyq__dirFailAuto(ctx_t *ctx) { RET(0); }
void psyq__padGetActSize(ctx_t *ctx) { RET(2); }
void psyq__padLoadActInfo(ctx_t *ctx) { RET(1); }
void psyq__padRecvAtLoadInfo(ctx_t *ctx) { RET(1); }

void psyq__padSendAtLoadInfo(ctx_t *ctx) {
    u32 port = A0 & 1u, data = A1;
    if (data) {
        ac3_pad[port].rumble_small = (u8)MEM_R8U(data);
        ac3_pad[port].rumble_large = (u8)MEM_R8U(data + 1);
    }
    RET(1);
}

void ac3_pad_machine_save(FILE *f) {
    ac3_ms_tag(f, "PAD");
    for (int p = 0; p < 2; p++) {
        const ac3_pad_t *pad = &ac3_pad[p];
        ac3_ms_u32(f, (u32)pad->connected);
        ac3_ms_u32(f, pad->type);
        ac3_ms_u32(f, pad->buttons);
        ac3_ms_u32(f, pad->lx);
        ac3_ms_u32(f, pad->ly);
        ac3_ms_u32(f, pad->rx);
        ac3_ms_u32(f, pad->ry);
        ac3_ms_u32(f, pad->rumble_small);
        ac3_ms_u32(f, pad->rumble_large);
    }
    for (int p = 0; p < 2; p++) {
        ac3_ms_u32(f, pad_buf[p]);
        ac3_ms_u32(f, pad_len[p]);
    }
    ac3_ms_u32(f, (u32)pad_started);
}
