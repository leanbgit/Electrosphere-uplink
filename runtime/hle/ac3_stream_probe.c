#include "hle.h"
#include "ac3_stream_probe.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

extern u32 ac3_vblank_count(void);

#define REQ_FLAGS      0
#define REQ_START_LBA 16
#define REQ_REMAIN    20
#define REQ_FILT_FILE 28
#define REQ_FILT_CHAN 29
#define REQ_STATE     32
#define REQ_CUR_LBA   36
#define REQ_END_LBA   44

#define RING_HDR_WORDS  0
#define RING_PAY_WORDS  4
#define RING_CAPACITY  12
#define RING_READY     52

int ac3_probe_verbose = -1;

static int probe_verbose(void) {
    if (ac3_probe_verbose < 0) {
        const char *e = getenv("AC3_PROBE");
        ac3_probe_verbose = (e && *e && *e != '0') ? 1 : 0;
    }
    return ac3_probe_verbose;
}

static FILE *out;

static void probe_open(void) {
    if (out) return;
    MKDIR("capture");
    out = fopen("capture/stream_probe.txt", "w");
    if (out)
        fprintf(out,
            "# KCd stream requests, one block per CdlReadN/CdlReadS.\n"
            "# 'descriptor' is the table entry the request was built from;\n"
            "# 'delivered' is what the disc actually handed back.\n"
            "# A black video window is a stream whose wanted channel carries\n"
            "# no data sectors -- see the VERDICT line.\n\n");
}

static void say(const char *fmt, ...) {
    va_list ap;
    probe_open();
    va_start(ap, fmt);
    if (out) { vfprintf(out, fmt, ap); fputc('\n', out); }
    va_end(ap);
    va_start(ap, fmt);
    fputs("[probe] ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    if (out) fflush(out);
}

static int rd32(u32 addr, u32 *v) {
    const void *p = guest_ptr(addr, 4);
    if (!p) return 0;
    *v = MEM_R32(addr);
    return 1;
}

static int rd16(u32 addr, u16 *v) {
    const void *p = guest_ptr(addr, 2);
    if (!p) return 0;
    *v = (u16)MEM_R16U(addr);
    return 1;
}

static int rd8(u32 addr, u8 *v) {
    const void *p = guest_ptr(addr, 1);
    if (!p) return 0;
    *v = (u8)MEM_R8U(addr);
    return 1;
}

static unsigned stream_seq;
static int      in_stream;
static u32      start_lba, want_chan;
static u32      sec_total, sec_data, sec_audio, sec_other;
static u32      data_by_chan[32];
static u32      hdr_reads, payload_reads;
static u32      rec_total, rec_video, rec_other;
static u32      mdec_frames;
static int      have_first_video;
static u32      fv_w, fv_h, fv_left, fv_chunks;
static u32      last_hdr_chan;

static void counters_reset(void) {
    sec_total = sec_data = sec_audio = sec_other = 0;
    memset(data_by_chan, 0, sizeof data_by_chan);
    hdr_reads = payload_reads = 0;
    rec_total = rec_video = rec_other = 0;
    mdec_frames = 0;
    have_first_video = 0;
    fv_w = fv_h = fv_left = fv_chunks = 0;
    last_hdr_chan = 0xFF;
}

typedef struct {
    int ok;
    u32 table, entry, packed, elba;
    u32 kind, shift, chan, field;
} desc_t;

static desc_t resolve_id(u32 gp, u32 id) {
    desc_t d;
    u32 p52;
    memset(&d, 0, sizeof d);
    if (!rd32(gp + 52, &p52) ||
        !rd32(p52 + ((id & 0x800u) ? 28u : 24u), &d.table))
        return d;
    d.entry = d.table + 8u * (id & 0x7FFu);
    if (!rd32(d.entry, &d.packed) || !rd32(d.entry + 4, &d.elba))
        return d;
    d.kind  = (d.packed >> 27) & 3u;
    d.shift = (d.packed >> 24) & 7u;
    d.chan  = (d.packed >> 19) & 0x1Fu;
    d.field = d.packed & 0x7FFFFu;
    d.ok = 1;
    return d;
}

static const char *kind_name(u32 kind) {
    switch (kind) {
    case 0:  return "plain read";
    case 1:  return "stream";
    case 2:  return "VOICE ONLY (no records)";
    default: return "video-capable";
    }
}

static void report_descriptor(u32 gp, u32 req_start, u32 req_len, u8 req_chan) {
    u32 p72, id_raw;
    if (!rd32(gp + 72, &p72) || !rd32(p72 + 372, &id_raw)) {
        say("   descriptor : id not readable");
        return;
    }
    u32 id = id_raw & 0xFFFFu;
    desc_t d = resolve_id(gp, id);
    if (!d.ok) {
        say("   descriptor : id 0x%04X did not resolve (table 0x%08X entry 0x%08X)",
            id, d.table, d.entry);
        return;
    }
    u32 kind = d.kind, shift = d.shift, chan = d.chan, field = d.field;
    u32 packed = d.packed, elba = d.elba;

    say("   descriptor : id 0x%04X  bank %u  index %u  table 0x%08X  entry 0x%08X",
        id, (id & 0x800u) ? 1u : 0u, id & 0x7FFu, d.table, d.entry);
    say("                packed 0x%08X  entry lba %u", packed, elba);
    say("                kind %u  shift %u  chan %u  field %u",
        kind, shift, chan, field);

    if (elba > req_start || req_start - elba > 60000u) {
        say("                entry lba is not below the request start --");
        say("                this stream was not started from this entry;");
        say("                the id above is stale, ignore the checks.");
        return;
    }

    u32 skip = req_start - elba;
    u32 explen = ((field - (skip >> shift)) & 0x7FFFFu) << shift;
    say("                skip %u  ->  len (field - skip>>shift) << shift = %u",
        skip, explen);
    say("   check      : channel  descriptor %u vs request %u   %s",
        chan, req_chan, chan == req_chan ? "match" : "MISMATCH");
    say("   check      : length   descriptor %u vs request %u   %s",
        explen, req_len, explen == req_len ? "match" : "MISMATCH");
}

void ac3_probe_stream_begin(ctx_t *ctx, u32 com, u32 lba,
                            u8 filter_file, u8 filter_chan) {
    u32 gp = ctx->r[28];
    u32 req = 0, ring = 0;
    u32 flags = 0, rstart = 0, rlen = 0, rend = 0, rstate = 0;
    u8  ffile = 0, fchan = 0;
    u32 hw = 0, pw = 0, cap = 0, ready = 0;

    if (in_stream) ac3_probe_stream_end(ctx, lba);

    stream_seq++;
    in_stream = 1;
    start_lba = lba;
    want_chan = filter_chan;
    counters_reset();

    say("");
    say("================================================================");
    say("stream #%u begin   frame %u   %s   lba %u",
        stream_seq, ac3_vblank_count(),
        com == 0x1B ? "CdlReadS" : "CdlReadN", lba);
    say("================================================================");

    if (rd32(gp + 56, &req) && req) {
        rd32(req + REQ_FLAGS, &flags);
        rd32(req + REQ_START_LBA, &rstart);
        rd32(req + REQ_REMAIN, &rlen);
        rd32(req + REQ_END_LBA, &rend);
        rd32(req + REQ_STATE, &rstate);
        rd8(req + REQ_FILT_FILE, &ffile);
        rd8(req + REQ_FILT_CHAN, &fchan);
        say("   request    : 0x%08X  flags 0x%08X  state %u", req, flags, rstate);
        say("                start lba %u  len %u  end lba %u",
            rstart, rlen, rend);
        say("                filter file %u chan %u   (drive filter said file %u chan %u)",
            ffile, fchan, filter_file, filter_chan);
        say("                match-by-channel %s",
            (flags & 0x1000u) ? "on (0x1000)" : "OFF -- takes any channel");
    } else {
        say("   request    : not readable via $gp+56");
    }

    if (rd32(gp + 64, &ring) && ring) {
        rd32(ring + RING_HDR_WORDS, &hw);
        rd32(ring + RING_PAY_WORDS, &pw);
        rd32(ring + RING_CAPACITY, &cap);
        rd32(ring + RING_READY, &ready);
        say("   ring       : 0x%08X  header %u words (%u bytes)  payload %u words (%u bytes)",
            ring, hw, hw * 4u, pw, pw * 4u);
        say("                capacity %u  records ready %u", cap, ready);
    }

    if (req) want_chan = fchan;
    report_descriptor(gp, rstart ? rstart : lba, rlen, fchan);
}

void ac3_probe_stream_end(ctx_t *ctx, u32 lba) {
    if (!in_stream) return;
    in_stream = 0;

    u32 gp = ctx->r[28], req = 0, ready = 0, ring = 0;
    rd32(gp + 64, &ring);
    if (ring) rd32(ring + RING_READY, &ready);
    rd32(gp + 56, &req);

    say("stream #%u end     frame %u   lba %u   (%u sectors read)",
        stream_seq, ac3_vblank_count(), lba,
        lba > start_lba ? lba - start_lba : 0);
    say("   delivered  : %u sectors   %u data, %u audio, %u other",
        sec_total, sec_data, sec_audio, sec_other);

    {
        char line[256];
        int n = 0;
        line[0] = 0;
        for (int c = 0; c < 32; c++) {
            if (!data_by_chan[c]) continue;
            n += snprintf(line + n, sizeof line - (size_t)n, "ch%d=%u ",
                          c, data_by_chan[c]);
            if (n >= (int)sizeof line - 16) break;
        }
        say("   data chans : %s", line[0] ? line : "(none)");
    }

    say("   wanted ch %u: %u data sectors", want_chan,
        want_chan < 32 ? data_by_chan[want_chan] : 0);
    say("   demux      : %u sector headers read, %u payloads taken",
        hdr_reads, payload_reads);
    say("   records    : %u total, %u video (type 3), %u other",
        rec_total, rec_video, rec_other);
    if (have_first_video)
        say("                first video record %ux%u  frames-left %u  chunks %u",
            fv_w, fv_h, fv_left, fv_chunks);
    say("   mdec       : %u frames decoded during this stream", mdec_frames);
    say("   ring ready : %u records left unconsumed", ready);

    if (want_chan < 32 && data_by_chan[want_chan] == 0)
        say("   VERDICT    : NO DATA SECTORS ON THE WANTED CHANNEL -- "
            "nothing can decode, window stays black");
    else if (rec_video == 0)
        say("   VERDICT    : data arrived on the wanted channel but no video "
            "records (type 3) -- wrong stream selected");
    else if (mdec_frames == 0)
        say("   VERDICT    : video records arrived but the decoder never ran "
            "-- fault is above the CD layer");
    else
        say("   VERDICT    : ok, %u video frames decoded", mdec_frames);
    say("");
}

void ac3_probe_sector(u32 lba, u8 file, u8 chan, u8 submode, int delivered) {
    (void)lba; (void)file; (void)delivered;
    if (!in_stream) return;
    sec_total++;
    if (submode & 0x08u) {
        sec_data++;
        if (chan < 32) data_by_chan[chan]++;
    } else if (submode & 0x04u) {
        sec_audio++;
    } else {
        sec_other++;
    }
}

void ac3_probe_read(u32 bytes, const u8 *data, u8 chan, u32 off) {
    if (!in_stream || !data) return;

    if (bytes == 12) {
        hdr_reads++;
        last_hdr_chan = chan;
        return;
    }
    payload_reads++;

    if (bytes >= 16 && off >= 12 && off < 64 && data[0] == 1) {
        u32 chunks = (u32)data[2] | ((u32)data[3] << 8);
        u32 type   = (u32)data[4] | ((u32)data[5] << 8);
        u32 left   = (u32)data[6] | ((u32)data[7] << 8);
        u32 w      = (u32)data[8] | ((u32)data[9] << 8);
        u32 h      = (u32)data[10] | ((u32)data[11] << 8);
        rec_total++;
        if (type == 3) {
            rec_video++;
            if (!have_first_video) {
                have_first_video = 1;
                fv_w = w; fv_h = h; fv_left = left; fv_chunks = chunks;
                say("   first video record: chan %u  %ux%u  frames-left %u  chunks %u",
                    last_hdr_chan, w, h, left, chunks);
            }
        } else {
            rec_other++;
        }
    }
}

void ac3_probe_mdec_in(u32 words, int bpp24) {
    (void)words; (void)bpp24;
    if (in_stream) mdec_frames++;
}

#define CMD_SLOTS      16
#define CMD_SLOT_SIZE  20

static const struct { u32 addr; const char *what; } cmd_handlers[] = {
    { 0x8002BDC4u, "op0  (trivial)" },
    { 0x8002BDCCu, "op1  (trivial)" },
    { 0x8002BE14u, "op2  stream, id inline, no flags" },
    { 0x8002BEE4u, "op3  stream, parameter list  <- voice" },
    { 0x8002C1D4u, "op4  stream, id inline + flags  <- video" },
    { 0x8002C2E4u, "op5  fade out, then stop" },
    { 0x8002C41Cu, "op6  pause / hold" },
};
#define CMD_HANDLER_B 0x8002BEE4u

static const char *handler_name(u32 addr) {
    for (unsigned i = 0; i < sizeof cmd_handlers / sizeof cmd_handlers[0]; i++)
        if (cmd_handlers[i].addr == addr) return cmd_handlers[i].what;
    return "?";
}

static int is_cmd_handler(u32 addr) {
    for (unsigned i = 0; i < sizeof cmd_handlers / sizeof cmd_handlers[0]; i++)
        if (cmd_handlers[i].addr == addr) return 1;
    return 0;
}

static u32 last_cmd_block, last_target, last_id;

static void report_entry(u32 gp, int n, u32 addr, u32 id, s32 skip,
                         int have_skip, const char *extra) {
    desc_t d = resolve_id(gp, id);
    if (!d.ok) {
        say("      #%-2d 0x%08X  id 0x%04X  did not resolve", n, addr, id);
        return;
    }
    u32 len = have_skip
            ? (((d.field - ((u32)(skip >> d.shift))) & 0x7FFFFu) << d.shift)
            : (d.field << d.shift);
    say("      #%-2d 0x%08X  id 0x%04X  bank %u index %-4u | kind %u chan %-2u "
        "lba %-8u len %-7u  %s%s",
        n, addr, id, (id & 0x800u) ? 1u : 0u, id & 0x7FFu,
        d.kind, d.chan, d.elba, len, kind_name(d.kind), extra);
}

void ac3_probe_script(ctx_t *ctx, u32 target) {
    if (!is_cmd_handler(target)) return;

    u32 cmd = ctx->r[4];
    u8  state = 0;
    if (!rd8(cmd + 1, &state) || state != 0) return;

    u32 gp = ctx->r[28];
    u16 inline_id = 0;
    u32 params = 0, gate = 0, argb = 0, flagw = 0;
    rd16(cmd + 2, &inline_id);
    rd32(cmd + 4, &params);
    rd32(cmd + 8, &gate);
    rd32(cmd + 12, &flagw);
    argb = params;

    u32 ident = (target == CMD_HANDLER_B) ? params : inline_id;
    if (cmd == last_cmd_block && target == last_target && ident == last_id)
        return;
    last_cmd_block = cmd;
    last_target = target;
    last_id = ident;

    u8 opcode = 0;
    rd8(cmd + 0, &opcode);
    say("");
    say("   script     : running op %u  handler 0x%08X  slot 0x%08X  [%s]",
        opcode, target, cmd, handler_name(target));

    {
        char line[80];
        int n = 0;
        line[0] = 0;
        for (u32 i = 0; i < 20; i++) {
            u8 b = 0;
            if (!rd8(cmd + i, &b)) break;
            n += snprintf(line + n, sizeof line - (size_t)n, "%02X ", b);
        }
        say("   slot bytes : %s", line);
    }

    if (target != CMD_HANDLER_B) {
        say("   command    : id inline at +2, arg 0x%08X arg2 0x%08X flags 0x%08X",
            argb, gate, flagw);
        report_entry(gp, 0, cmd + 2, inline_id, 0, 0, "   <- this one starts now");
        return;
    }

    if (!params) { say("   command    : no parameter block"); return; }
    say("   command    : parameter list at 0x%08X  gate 0x%08X", params, gate);

    u32 p = params;
    for (int n = 0; n < 16 && p; n++) {
        u16 raw = 0, flag = 0, dur = 0, skipw = 0;
        u32 next = 0;
        if (!rd16(p + 0, &raw) || !rd16(p + 2, &flag) ||
            !rd16(p + 4, &dur) || !rd16(p + 6, &skipw) || !rd32(p + 8, &next)) {
            say("      #%-2d block 0x%08X unreadable", n, p);
            break;
        }
        u32 id = raw;
        if (!gate) id &= ~0x1000u;
        report_entry(gp, n, p, id, (s32)(s16)skipw, 1,
                     n == 0 ? "   <- this one starts now" : "");
        p = next;
    }
}

static u32 q_reported;
static int q_primed;
static u32 q_last_imm;

static void dump_slot(u32 gp, const char *why, u32 slot, long index) {
    u8 opcode = 0, state = 0;
    u16 inline_id = 0;
    u32 a1 = 0, a2 = 0, flags = 0, done = 0;

    if (!rd8(slot + 0, &opcode)) { say("   queued     : slot 0x%08X unreadable", slot); return; }
    rd8(slot + 1, &state);
    rd16(slot + 2, &inline_id);
    rd32(slot + 4, &a1);
    rd32(slot + 8, &a2);
    rd32(slot + 12, &flags);
    rd32(slot + 16, &done);

    u32 haddr = opcode < sizeof cmd_handlers / sizeof cmd_handlers[0]
              ? cmd_handlers[opcode].addr : 0;

    if (index >= 0)
        say("   %s : #%ld  slot 0x%08X  op %u  [%s]",
            why, index, slot, opcode,
            haddr ? handler_name(haddr) : "opcode out of range");
    else
        say("   %s : slot 0x%08X  op %u  [%s]", why, slot, opcode,
            haddr ? handler_name(haddr) : "opcode out of range");
    say("                state %u  id 0x%04X  args 0x%08X 0x%08X  flags 0x%08X  done 0x%08X",
        state, inline_id, a1, a2, flags, done);

    if (haddr == CMD_HANDLER_B) {
        u16 raw = 0;
        if (a1 && rd16(a1, &raw)) {
            u32 id = raw;
            if (!a2) id &= ~0x1000u;
            report_entry(gp, 0, a1, id, 0, 0, "");
        }
    } else if (haddr == 0x8002BE14u || haddr == 0x8002C1D4u) {
        report_entry(gp, 0, slot + 2, inline_id, 0, 0, "");
    }
}

#define UNK_MAX 32
static struct { u32 word, pc, hits; } unk[UNK_MAX];
static unsigned unk_count;
static unsigned long unk_total;

void ac3_probe_unknown_insn(u32 pc, u32 word) {
    unk_total++;
    for (unsigned i = 0; i < unk_count; i++) {
        if (unk[i].word == word) {
            unk[i].hits++;
            if (unk[i].hits == 10 || unk[i].hits == 1000 ||
                unk[i].hits == 100000)
                say("   interp     : unknown 0x%08X now executed %u times "
                    "(as a no-op)", word, unk[i].hits);
            return;
        }
    }
    if (unk_count < UNK_MAX) {
        unk[unk_count].word = word;
        unk[unk_count].pc = pc;
        unk[unk_count].hits = 1;
        unk_count++;
        say("   interp     : UNKNOWN INSTRUCTION 0x%08X at 0x%08X "
            "(op 0x%02X funct 0x%02X) -- executed as a no-op",
            word, pc, word >> 26, word & 0x3F);
    } else if (unk_total == UNK_MAX + 1) {
        say("   interp     : more than %d distinct unknown instructions; "
            "no longer listing them", UNK_MAX);
    }
}

static u32 q_base, q_gp;
extern u32 ac3_interp_pc;
extern u32 ac3_cur_func;

#define MAX_RAM_DUMPS 12
static unsigned ram_dumps;

static void dump_ram(long index, unsigned opcode) {
    char path[256];
    if (ram_dumps >= MAX_RAM_DUMPS) {
        if (ram_dumps == MAX_RAM_DUMPS) {
            ram_dumps++;
            say("   overlay    : %d RAM images written, no more will be taken",
                MAX_RAM_DUMPS);
        }
        return;
    }
    ram_dumps++;

    MKDIR("capture");
    snprintf(path, sizeof path, "capture/ram_q%02ld_op%u.bin", index, opcode);
    FILE *f = fopen(path, "wb");
    if (!f) { say("   overlay    : cannot write %s", path); return; }
    fwrite(ac3_ram, 1, AC3_RAM_SIZE, f);
    fclose(f);
    say("   overlay    : RAM image -> %s  (load at 0x80000000, %u bytes)",
        path, (unsigned)AC3_RAM_SIZE);
}

static u32 vw_lo, vw_hi;

void ac3_probe_write_hit(u32 addr, u32 value, int size) {
    if (!vw_hi || addr < vw_lo || addr >= vw_hi) return;
    static unsigned shown;
    u32 off = addr - vw_lo;
    if (off > 0x38u) return;
    if (shown++ > 40u) return;
    say("   ctxwrite   : +0x%02X <- 0x%08X (%d bytes)  by %s 0x%08X",
        off, value, size,
        ac3_interp_pc ? "interpreted overlay pc" : "compiled func",
        ac3_interp_pc ? ac3_interp_pc : ac3_cur_func);
}

static const struct { u32 addr; const char *what; } overlay_calls[] = {
    { 0x800FCE40u, "video window init (allocates the context)" },
    { 0x800FD028u, "video window state machine" },
    { 0x800FC854u, "video command enqueue helper" },
    { 0x800F072Cu, "voice command enqueue helper" },
    { 0x800EDCC0u, "type-2 portrait: rect + SetDrawMode" },
    { 0x800EC6B4u, "type-2 callee (from 0x800EDDD0)" },
    { 0x800EDC80u, "type-2 callee (from 0x800EDEA8)" },
    { 0x800F7244u, "type-2 callee (from 0x800EE0B4)" },
    { 0x800DEF94u, "type-2 callee (from 0x800EDE44/50)" },
    { 0x800DEE08u, "type-2 callee (from 0x800EDE64/78)" },
};

static const struct { u32 pc; const char *what; } interp_bp[] = {
    { 0x800F0484u, "type = entry->byte[7]   (v0 = entry, v1 = type after this)" },
    { 0x800F04B0u, "guard: create video window only if type == 1" },
    { 0x800F04C0u, "phase = 0x4000 -- video window WILL be created" },
};

void ac3_probe_interp_pc(ctx_t *ctx, u32 pc) {
    if ((pc & 0xFFFFF000u) != 0x800F0000u || !probe_verbose()) return;
    for (unsigned i = 0; i < sizeof interp_bp / sizeof interp_bp[0]; i++) {
        if (interp_bp[i].pc != pc) continue;
        say("   ovlbp      : 0x%08X  %s", pc, interp_bp[i].what);
        say("                v0=0x%08X v1=0x%08X a2=0x%08X a3=0x%08X s1=0x%08X",
            ctx->r[2], ctx->r[3], ctx->r[6], ctx->r[7], ctx->r[17]);
        if (pc == 0x800F0484u) {
            u8 t = 0;
            if (rd8(ctx->r[2] + 7u, &t))
                say("                entry 0x%08X  type byte = %u   %s",
                    ctx->r[2], t,
                    t == 1u ? "== 1, this screen gets a video window"
                            : "!= 1, NO video window for this screen");
        }
        return;
    }
}

void ac3_probe_unhandled_packet(u32 addr, u8 code, int len) {
    static struct { u8 code; u32 hits; } seen[16];
    static unsigned seen_n;
    for (unsigned i = 0; i < seen_n; i++) {
        if (seen[i].code != code) continue;
        seen[i].hits++;
        if (seen[i].hits == 100 || seen[i].hits == 10000)
            say("   gpupacket  : code 0x%02X seen %u times, still dropped",
                code, seen[i].hits);
        return;
    }
    if (seen_n >= 16) return;
    seen[seen_n].code = code;
    seen[seen_n].hits = 1;
    seen_n++;

    say("   gpupacket  : UNHANDLED code 0x%02X at 0x%08X len %d -- dropped",
        code, addr, len);
    if (code == 0xA0u) {
        u32 dst = 0, dim = 0;
        rd32(addr + 4, &dst);
        rd32(addr + 8, &dim);
        say("                Copy CPU->VRAM  dest (%u,%u)  %ux%u"
            "   <== a texture upload is being discarded",
            dst & 0xFFFFu, (dst >> 16) & 0xFFFFu,
            dim & 0xFFFFu, (dim >> 16) & 0xFFFFu);
    }
}

void ac3_probe_load_image(int x, int y, int w, int h, u32 src) {
    if (x + w <= 832 || x >= 1024 || y + h <= 240 || y >= 400) return;
    if (!probe_verbose()) return;
    say("   winupload  : vram (%4d,%4d) %3dx%-3d  <- guest 0x%08X   frame %u",
        x, y, w, h, src, ac3_vblank_count());
}

void ac3_probe_overlay_call(ctx_t *ctx, u32 target) {
    if (!probe_verbose()) return;
    for (unsigned i = 0; i < sizeof overlay_calls / sizeof overlay_calls[0]; i++) {
        if (overlay_calls[i].addr != target) continue;
        u32 ctx_ptr = 0, state = 0;
        rd32(0x800BD044u, &ctx_ptr);
        if (ctx_ptr >= 0x80000000u && ctx_ptr < 0x80200000u) rd32(ctx_ptr, &state);
        say("   ovlcall    : 0x%08X  %s   (ctx 0x%08X state %u)",
            target, overlay_calls[i].what, ctx_ptr, state);
        return;
    }
    (void)ctx;
}

void ac3_probe_queue(ctx_t *ctx) {
    u32 gp = ctx->r[28], q = 0, wr = 0, imm = 0;
    if (!rd32(gp + 72, &q) || !q) return;
    if (!rd32(q + 4, &wr)) return;

    if (!q_base) { q_base = q; q_gp = gp; }

    if (!q_primed) {
        q_primed = 1;
        q_reported = wr;
        q_last_imm = 0;
    }

    if (wr - q_reported > CMD_SLOTS) {
        say("   queue      : %u commands queued since the last look -- the ring "
            "wrapped, some were overwritten before they could be read",
            wr - q_reported);
        q_reported = wr - CMD_SLOTS;
    }
    while (q_reported != wr) {
        unsigned i = q_reported & (CMD_SLOTS - 1);
        u32 slot = q + 8 + CMD_SLOT_SIZE * i;
        dump_slot(gp, "queued    ", slot, (long)q_reported);

        {
            u8 opcode = 0;
            if (rd8(slot, &opcode) && (opcode == 3 || opcode == 4))
                dump_ram((long)q_reported, opcode);
        }
        q_reported++;
    }

    {
        static u32 last_ctx, last_state;
        static int vs_seen;
        u32 ctx_ptr = 0, state = 0;
        if (rd32(0x800BD044u, &ctx_ptr)) {
            int live = (ctx_ptr >= 0x80000000u && ctx_ptr < 0x80200000u);
            if (live) rd32(ctx_ptr, &state);
            if (live && !vw_hi && !ac3_watch_hi && probe_verbose()) {
                vw_lo = ctx_ptr;
                vw_hi = ctx_ptr + 0x400u;
                ac3_watch_lo = vw_lo;
                ac3_watch_hi = vw_hi;
                say("   videowin   : watching context 0x%08X..0x%08X for writes",
                    vw_lo, vw_hi);
            }
            if (!vs_seen || ctx_ptr != last_ctx || state != last_state) {
                vs_seen = 1;
                u32 id = 0;
                if (live) rd32(ctx_ptr + 0x34, &id);
                say("   videowin   : ctx 0x%08X  state %u  id 0x%04X   %s",
                    ctx_ptr, state, id & 0xFFFFu,
                    !live            ? "pointer not in RAM" :
                    state >= 5u      ? "STATE OUT OF RANGE -- machine returns, "
                                       "no video command"
                                     : (state == 0 ? "about to queue the video"
                                                   : "running"));
                last_ctx = ctx_ptr;
                last_state = state;
            }
        }
    }

    if (rd32(q + 328, &imm) && imm && imm != q_last_imm)
        dump_slot(gp, "immediate ", q + 332, -1);
    q_last_imm = imm;
}
