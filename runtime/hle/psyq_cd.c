/*
 * psyq_cd.c -- the CD drive, as the game's library (libcd, libds) sees it.
 *
 * Commands (CdControl) move the head and start or stop reading. Reads are
 * either blocks at once (CdRead) or a stream: sectors arrive one by one at
 * the drive's speed (ac3_cd_tick, every field), each announced by the ready
 * callback and fetched with CdGetSector; audio sectors (XA) go to the sound
 * instead. The discs themselves are in cd_discs.c and cd_image.c.
 */
#include "cd_internal.h"
#include "ac3_capture.h"
#include "ac3_stream_probe.h"
#include "ac3_oracle.h"
#include "xa_adpcm.h"
#include <string.h>
#include <stdlib.h>

#define CD_MODE_SIZE0 0x10
#define CD_MODE_SIZE1 0x20
#define CD_MODE_RT    0x40

enum {
    C_Nop = 0x01, C_Setloc = 0x02, C_Play = 0x03, C_Forward = 0x04,
    C_Backward = 0x05, C_ReadN = 0x06, C_Standby = 0x07, C_Stop = 0x08,
    C_Pause = 0x09, C_Mute = 0x0B, C_Demute = 0x0C, C_Setfilter = 0x0D,
    C_Setmode = 0x0E, C_Getparam = 0x0F, C_GetlocL = 0x10, C_GetlocP = 0x11,
    C_GetTN = 0x13, C_GetTD = 0x14, C_SeekL = 0x15, C_SeekP = 0x16,
    C_ReadS = 0x1B,
};

#define EV_CLASS_CD   0xF0000003u
#define EV_SPEC_COMPLETE 0x0020u
#define EV_SPEC_READY    0x0040u

extern void ac3_deliver_event(ctx_t *ctx, u32 cls, u32 spec);

static int streaming;           /* reading (ReadN/ReadS) */
static u32   cur_lba;           /* the head */
static u32   cd_mode;           /* Setmode */

static int filter_active;        /* Setfilter: the XA file and channel to play */
static u8  filter_file, filter_chan;

static u8  consumed_chan;        /* the channel of the last data the game took */
static int consumed_known;

#define CD_MODE_SF    0x08

static int   sector_credit;      /* drive time owed, in sectors x 60 (ac3_cd_tick) */
int ac3_cd_rate;                 /* --cd-rate: sectors per second (0: the mode's speed) */

/* ---- the sector being delivered ------------------------------------------- */

/* where the game's data starts in a whole sector (the mode's sector size) */
static u32 delivery_offset(void) {
    if (cd_mode & CD_MODE_SIZE1) return 0;
    if (cd_mode & CD_MODE_SIZE0) return 4;
    return 12;
}

static u8  fifo[WHOLE_SIZE];
static u32 fifo_lba;
static int fifo_valid;
static u32 fifo_pos;

static int fifo_step(void) {
    if (!read_sector_whole(cur_lba, fifo)) return -1;
    fifo_lba = cur_lba;
    cur_lba++;

    {
        u8 sub = fifo[SUB_MODE];
        ac3_probe_sector(fifo_lba, fifo[SUB_FILE], fifo[SUB_CHAN], sub,
                         (sub & SUBMODE_DATA) != 0);
        if (!(sub & SUBMODE_DATA)) {
            ac3_xa_sector(fifo);
            return 0;
        }
    }
    fifo_valid = 1;
    fifo_pos = delivery_offset();
    return 1;
}

static int fifo_fill(void) {
    for (int tries = 0; tries < 4096; tries++) {
        int r = fifo_step();
        if (r < 0) return 0;
        if (r > 0) return 1;
    }
    return 0;
}

/* ---- the library ---------------------------------------------------------- */

void psyq_CdInit(ctx_t *ctx) {
    if (!disc_mounted()) hle_warn("CdInit", "no disc image mounted");
    cur_lba = 0;
    cd_mode = 0;
    RET(disc_mounted() ? 1 : 0);
}

#define STAT_ERROR   0x01
#define STAT_MOTOR   0x02
#define STAT_SHELL   0x10
#define STAT_READING 0x20
#define STAT_SEEKING 0x40
#define STAT_PLAYING 0x80

static u8 drive_status(void) {
    u8 s = disc_mounted() ? STAT_MOTOR : (STAT_SHELL | STAT_ERROR);
    if (disc_mounted() && streaming) s |= STAT_READING;
    return s;
}

static void write_status(u32 result) {
    if (!result) return;
    u8 *r = (u8 *)guest_ptr(result, 8);
    if (!r) return;
    memset(r, 0, 8);
    r[0] = drive_status();
}

static u32 sync_callback_fn;
static u32 ready_callback_fn;

#define CdlDataReady 1
#define CdlComplete  2
#define CdlDataEnd   3
#define CdlAcknowledge 4

static void invoke_cb(ctx_t *ctx, u32 fn, u32 intr, u32 result) {
    if (!fn) return;
    u32 sa0 = ctx->r[4], sa1 = ctx->r[5];
    ctx->r[4] = intr;
    ctx->r[5] = result;
    ac3_call_indirect(ctx, fn);
    ctx->r[4] = sa0;
    ctx->r[5] = sa1;
}

void psyq_CdSyncCallback(ctx_t *ctx) {
    u32 prev = sync_callback_fn;
    sync_callback_fn = A0;
    hle_trace("CdSyncCallback", "fn=0x%08X", A0);
    RET(prev);
}

void psyq_CdReadyCallback(ctx_t *ctx) {
    u32 prev = ready_callback_fn;
    ready_callback_fn = A0;
    hle_trace("CdReadyCallback", "fn=0x%08X", A0);
    RET(prev);
}

void psyq_CD_init(ctx_t *ctx)     { RET(0); }
void psyq_CD_initintr(ctx_t *ctx) { RET(0); }
void psyq_CD_initvol(ctx_t *ctx)  { RET(0); }
void psyq_CD_flush(ctx_t *ctx)    { RET(0); }

void psyq_CD_sync(ctx_t *ctx) {
    write_status(A1);
    RET(2);
}

void psyq_CD_ready(ctx_t *ctx) {
    write_status(A1);
    RET(streaming ? 1 : 0);
}
void psyq_CD_datasync(ctx_t *ctx) { RET(0); }
void psyq_CD_vol(ctx_t *ctx)      { RET(0); }
void psyq_CD_set_test_parmnum(ctx_t *ctx) { RET(0); }
void psyq_CD_getsector(ctx_t *ctx) {
    hle_trace("CD_getsector", "a0=0x%08X a1=0x%08X", A0, A1);
    RET(0);
}
void psyq_CD_cw(ctx_t *ctx) {
    hle_trace("CD_cw", "com=0x%02X a1=0x%08X a2=0x%08X a3=0x%08X",
              A0 & 0xFF, A1, A2, A3);
    RET(0);
}
void psyq_CdMix(ctx_t *ctx)       { RET(1); }
void psyq_DsMix(ctx_t *ctx)       { RET(1); }
void psyq_CdSync(ctx_t *ctx) {
    write_status(A1);
    RET(2);
}

void psyq_CdPosToInt(ctx_t *ctx) {
    u8 *loc = (u8 *)guest_ptr(A0, 4);
    if (!loc) { RET(0); return; }
    RET(msf_to_lba_signed(loc[0], loc[1], loc[2]));
}

void psyq_DsPosToInt(ctx_t *ctx) { psyq_CdPosToInt(ctx); }

void psyq_CdIntToPos(ctx_t *ctx) {
    u8 *loc = (u8 *)guest_ptr(A1, 4);
    if (loc) {
        lba_to_msf(A0, &loc[0], &loc[1], &loc[2]);
        loc[3] = 0;
    }
    RET(A1);
}

void psyq_CdSearchFile(ctx_t *ctx) {
    u8 *fp = (u8 *)guest_ptr(A0, 24);
    char name[64];
    u32 na = A1;
    int n = 0;
    while (n < (int)sizeof name - 1) {
        u8 c = (u8)MEM_R8U(na + (u32)n);
        if (c == 0) break;
        name[n++] = (char)c;
    }
    name[n] = 0;

    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char clean[64];
    int m = 0;
    for (const char *p = base; *p && *p != ';' && m < 63; p++) clean[m++] = *p;
    clean[m] = 0;

    u32 lba = 0, size = 0;
    if (!fp || !find_file(clean, &lba, &size)) {
        hle_warn("CdSearchFile", "not found: %s", clean);
        RET(0);
        return;
    }
    lba_to_msf(lba, &fp[0], &fp[1], &fp[2]);
    fp[3] = 0;
    memcpy(fp + 4, &size, 4);
    memset(fp + 8, 0, 16);
    memcpy(fp + 8, clean, (size_t)(m < 15 ? m : 15));
    hle_trace("CdSearchFile", "%s -> lba %u size %u", clean, lba, size);
    RET(A0);
}

static void cd_command(ctx_t *ctx, u32 com, u32 param, u32 result);

void psyq_CdControl(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, A2);
    RET(1);
}

void psyq_CdControlB(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, A2);
    RET(1);
}

void psyq_CdControlF(ctx_t *ctx) {
    cd_command(ctx, A0 & 0xFF, A1, 0);
    invoke_cb(ctx, sync_callback_fn, CdlComplete, 0);
    RET(1);
}

static void cd_command(ctx_t *ctx, u32 com, u32 param, u32 result) {
    switch (com) {
    case C_Setloc: {
        u8 *loc = (u8 *)guest_ptr(param, 4);
        if (loc) {
            u32 lba = msf_to_lba(loc[0], loc[1], loc[2]);
            if (lba != cur_lba) {
                cur_lba = lba;
                fifo_valid = 0;
                consumed_known = 0;
                if (sector_credit > 0) sector_credit = 0;
            }
        }
        break;
    }
    case C_Setmode:
        cd_mode = param ? MEM_R8U(param) : 0;
        ac3_xa_set_enabled((cd_mode & CD_MODE_RT) != 0);
        break;
    case C_GetTN:
        if (result) { MEM_W8(result, 0x01); MEM_W8(result + 1, 0x01); }
        break;
    case C_GetTD:
        if (result) {
            u8 m, s, f;
            lba_to_msf(drive_sectors(), &m, &s, &f);
            MEM_W8(result, m); MEM_W8(result + 1, s); MEM_W8(result + 2, f);
        }
        break;
    case C_GetlocL:
    case C_GetlocP:
        if (result) {
            u8 m, s, f;
            lba_to_msf(cur_lba, &m, &s, &f);
            MEM_W8(result, m); MEM_W8(result + 1, s); MEM_W8(result + 2, f);
            MEM_W8(result + 3, 1);
        }
        break;
    case C_ReadN:
    case C_ReadS:
        pack_note_read(cur_lba);
        streaming = 1;
        ac3_probe_stream_begin(ctx, com, cur_lba, filter_file, filter_chan);
        break;
    case C_Pause:
    case C_Stop:
    case C_Standby:
        streaming = 0;
        ac3_xa_stop();
        ac3_probe_stream_end(ctx, cur_lba);
        break;
    case C_Mute:
        ac3_xa_set_muted(1);
        break;
    case C_Demute:
        ac3_xa_set_muted(0);
        break;
    case C_Setfilter: {
        const u8 *f = (const u8 *)guest_ptr(param, 2);
        if (f) {
            filter_file = f[0];
            filter_chan = f[1];
            filter_active = 1;
            ac3_xa_set_filter(1, filter_file, filter_chan);
            hle_trace("CdControl", "Setfilter file=%u chan=%u",
                      filter_file, filter_chan);
        }
        break;
    }
    case C_SeekL:
    case C_SeekP:
    case C_Nop:
        break;
    default:
        hle_warn("CdControl", "unhandled command 0x%02X", com);
        break;
    }
    if (com != C_GetTN && com != C_GetTD && com != C_GetlocL && com != C_GetlocP)
        write_status(result);
    {
        extern u32 ac3_cur_func;
        hle_trace("CdCommand", "com=0x%02X param=0x%08X -> lba %u  from func 0x%08X",
                  com, param, cur_lba, ac3_cur_func);
    }
    ac3_deliver_event(ctx, EV_CLASS_CD, EV_SPEC_COMPLETE);
}

void psyq_CdRead(ctx_t *ctx) {
    u32 sectors = A0, dst = A1;
    cd_mode = A2;
    u8 buf[USER_SIZE];
    u32 ok = 0;
    pack_note_read(cur_lba);
    for (u32 i = 0; i < sectors; i++) {
        if (!read_sector(cur_lba + i, buf)) {
            hle_warn("CdRead", "read failed at lba %u", cur_lba + i);
            break;
        }
        u8 *g = (u8 *)guest_ptr(dst + i * USER_SIZE, USER_SIZE);
        if (!g) { hle_warn("CdRead", "bad destination 0x%08X", dst); break; }
        memcpy(g, buf, USER_SIZE);
        ac3_watch_bulk(dst + i * USER_SIZE, USER_SIZE, "CdRead");
        ac3_oracle_cd(cur_lba + i, dst + i * USER_SIZE, USER_SIZE);
        ok++;
    }
    hle_trace("CdRead", "lba %u, %u sectors -> 0x%08X, first word 0x%08X",
              cur_lba, ok, dst, ok ? MEM_R32(dst) : 0);
    cur_lba += ok;
    ac3_deliver_event(ctx, EV_CLASS_CD, EV_SPEC_COMPLETE);
    RET(ok == sectors);
}

void psyq_CdReadSync(ctx_t *ctx) {
    if (A1) MEM_W8(A1, 0x02);
    RET(0);
}

void psyq_CdReadBreak(ctx_t *ctx) { RET(0); }

static u32 data_callback_fn;

void psyq_CdDataCallback(ctx_t *ctx) {
    u32 prev = data_callback_fn;
    data_callback_fn = A0;
    hle_trace("CdDataCallback", "fn=0x%08X", A0);
    RET(prev);
}
void psyq_DsDataCallback(ctx_t *ctx) { psyq_CdDataCallback(ctx); }

void psyq_CdGetSector(ctx_t *ctx) {
    u32 dst = A0, words = A1;
    u32 bytes = words * 4;

    if (!fifo_valid && !fifo_fill()) {
        hle_warn("CdGetSector", "read failed at lba %u", cur_lba);
        RET(0);
        return;
    }
    u8 *g = (u8 *)guest_ptr(dst, bytes);
    if (!g) { hle_warn("CdGetSector", "bad destination 0x%08X", dst); RET(0); return; }
    if (fifo_pos >= WHOLE_SIZE) fifo_pos = delivery_offset();
    if (bytes > WHOLE_SIZE - fifo_pos) bytes = WHOLE_SIZE - fifo_pos;
    memcpy(g, fifo + fifo_pos, bytes);
    ac3_watch_bulk(dst, bytes, "CdGetSector");
    const u8 *buf = fifo + fifo_pos;
    if (bytes >= 512) {
        consumed_chan = fifo[SUB_CHAN];
        consumed_known = 1;
        ac3_oracle_cd(fifo_lba, dst, bytes);
    }
    ac3_probe_read(bytes, buf, fifo[SUB_CHAN], fifo_pos);
    fifo_pos += bytes;
    if (fifo_pos >= WHOLE_SIZE) fifo_valid = 0;
    hle_trace("CdGetSector", "lba %u -> 0x%08X (%u bytes) data %02X %02X %02X %02X %02X %02X %02X %02X",
              fifo_lba, dst, bytes, buf[0], buf[1], buf[2], buf[3],
              buf[4], buf[5], buf[6], buf[7]);
    RET(1);
}
void psyq_CdGetSector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }
void psyq_DsGetSector(ctx_t *ctx)  { psyq_CdGetSector(ctx); }
void psyq_DsGetSector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }
void psyq_CD_getsector2(ctx_t *ctx) { psyq_CdGetSector(ctx); }

void psyq_CdReady(ctx_t *ctx) {
    if (streaming && !fifo_valid) fifo_fill();
    write_status(A1);
    RET((streaming && fifo_valid) ? 1 : 0);
}
void psyq_DS_ready(ctx_t *ctx) { psyq_CdReady(ctx); }

void psyq_CdDataSync(ctx_t *ctx) { RET(0); }
void psyq_DsDataSync(ctx_t *ctx) { RET(0); }
void psyq_DS_sync(ctx_t *ctx)    { RET(2); }

void psyq_CdReset(ctx_t *ctx)   { streaming = 0; cur_lba = 0; ac3_xa_reset(); RET(1); }
void psyq_CdFlush(ctx_t *ctx)   { RET(0); }
void psyq_CdLastPos(ctx_t *ctx) { RET(0); }

void psyq_CdComstr(ctx_t *ctx) { RET(0); }
void psyq_CdIntstr(ctx_t *ctx) { RET(0); }

#define CD_MODE_SPEED 0x80
#define FRAMES_PER_SEC 60

static int delivery_rate(void) {
    if (ac3_cd_rate > 0) return ac3_cd_rate;
    return (cd_mode & CD_MODE_SPEED) ? 150 : 75;
}

enum { DEL_END = -1, DEL_FILTERED = 0, DEL_DELIVERED = 1 };

static int deliver_one(ctx_t *ctx) {
    int r = fifo_step();
    if (r < 0) return DEL_END;
    if (r == 0) return DEL_FILTERED;
    invoke_cb(ctx, ready_callback_fn, CdlDataReady, 0);
    if (data_callback_fn) invoke_cb(ctx, data_callback_fn, CdlDataReady, 0);
    return DEL_DELIVERED;
}

static int fifo_ends_record(void) {
    if (consumed_known && fifo[SUB_CHAN] != consumed_chan) return 0;
    const u8 *p = fifo + 12;
    u32 chunks = (u32)p[2] | ((u32)p[3] << 8);
    return p[0] == 1 && chunks != 0 && (u32)p[1] + 1u >= chunks;
}

void ac3_cd_tick(ctx_t *ctx) {
    sector_credit += delivery_rate();
    if (!streaming) {
        if (sector_credit > 0) sector_credit = 0;
        return;
    }
    while (sector_credit >= FRAMES_PER_SEC) {
        int r = deliver_one(ctx);
        if (r == DEL_END) break;
        sector_credit -= FRAMES_PER_SEC;
        if (!streaming) break;
    }
}

void ac3_cd_stream_ahead(ctx_t *ctx) {
    if (!streaming) return;
    for (int i = 0; i < 256; i++) {
        int r = deliver_one(ctx);
        if (r == DEL_END) break;
        sector_credit -= FRAMES_PER_SEC;
        if (!streaming) break;
        if (r == DEL_DELIVERED && fifo_ends_record()) break;
    }
}

void ac3_cd_snapshot(ac3_cd_snap_t *s) {
    if (!s) return;
    s->lba = cur_lba;
    s->mode = cd_mode;
    s->streaming = streaming;
    s->credit = sector_credit;
    s->rate = delivery_rate();
    s->filter_active = filter_active;
    s->filter_file = filter_file;
    s->filter_chan = filter_chan;
    s->fifo_lba = fifo_lba;
    s->fifo_valid = fifo_valid;
}

int ac3_cd_debt_frames(void) {
    int rate = delivery_rate();
    if (sector_credit >= 0 || rate <= 0) return 0;
    return (-sector_credit + rate - 1) / rate;
}

void psyq_CdGetToc(ctx_t *ctx) {
    u32 out = A0;
    if (out) {
        u8 m, s, f;
        lba_to_msf(0, &m, &s, &f);
        MEM_W8(out, 0); MEM_W8(out + 1, m);
        MEM_W8(out + 2, s); MEM_W8(out + 3, f);
    }
    RET(1);
}

void ac3_cd_machine_save(FILE *f) {
    ac3_ms_tag(f, "CD");
    ac3_ms_u32(f, (u32)streaming);
    ac3_ms_u32(f, cur_lba);
    ac3_ms_u32(f, cd_mode);
    ac3_ms_u32(f, (u32)filter_active);
    ac3_ms_u32(f, filter_file);
    ac3_ms_u32(f, filter_chan);
    ac3_ms_u32(f, consumed_chan);
    ac3_ms_u32(f, (u32)consumed_known);
    ac3_ms_u32(f, (u32)sector_credit);
    ac3_ms_u32(f, (u32)ac3_cd_rate);
    ac3_ms_bytes(f, fifo, WHOLE_SIZE);
    ac3_ms_u32(f, fifo_lba);
    ac3_ms_u32(f, (u32)fifo_valid);
    ac3_ms_u32(f, fifo_pos);
    ac3_ms_u32(f, sync_callback_fn);
    ac3_ms_u32(f, ready_callback_fn);
    ac3_ms_u32(f, data_callback_fn);
    discs_machine_save(f);
}
