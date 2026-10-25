#include "ac3_oracle.h"
#include "ac3_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#define GAME_GP        0x800BCB74u
static const u32 sim_fns[] = {
    0x80032F3Cu,
    0x8003307Cu,
    0x800330CCu,
    0x80033104u,
    0x800331E0u,
    0x8003329Cu,
};
#define CAM_STATE_PTR  0x800BD098u
#define RAND_SEED      0x800CBF58u

#define SCENE_BYTES    0x1100u
#define WORLD_BYTES    700u
#define CAMERA_BYTES   64u

extern u32 ac3_vblank_count(void);
extern u16 *ac3_vram;

static struct {
    int enabled;
    int traced;
    int blocks;
    u32 ram_every;
    u32 state_from, state_to, state_every;
    FILE *state;
    char dir[512];
    FILE *frames, *ticks, *ticks_bin, *cd, *vram;
    FILE *rec, *play;
    int play_done, play_warned;
    u32 tick;
    u32 n_upload, n_copy, n_store, n_fill;
    int run_open;
    u32 run_vblank, run_lba0, run_lba, run_dst0, run_dst, run_bytes;
    int rand_callers, in_draw;
    u32 draw_frames;
    u32 rand_ra[64], rand_n[64];
    u32 machine_tick, machine_field;
    int machine_done, machine_phases;
    u32 last_phase;
    u32 frames_from, frames_to;
} O;

static u64 hash_bytes(const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    u64 h = 0x9E3779B97F4A7C15ull ^ (u64)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, b + i, 8);
        h ^= w;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

static u64 hash_guest(u32 addr, u32 len) {
    const void *p = guest_ptr(addr, len);
    return p ? hash_bytes(p, len) : 0;
}

static u64 hash_vram_rect(const PSX_RECT *r) {
    u64 h = 0xCBF29CE484222325ull;
    if (!ac3_vram) return 0;
    for (s32 y = 0; y < r->h; y++) {
        const u16 *row = ac3_vram + (u32)((r->y + y) & 511) * GPU_VRAM_W;
        for (s32 x = 0; x < r->w; x++) {
            h ^= row[(r->x + x) & 1023];
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

static u32 guest_word_ptr(u32 addr) {
    u32 v = MEM_R32(addr);
    return ((v >> 24) == 0x80u && (v & 0x1FFFFFu) < AC3_RAM_SIZE) ? v : 0;
}

static void make_dirs(const char *path, int file) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    size_t n = strlen(buf);
    if (file) {
        while (n && buf[n - 1] != '/' && buf[n - 1] != '\\') n--;
        buf[n] = '\0';
    }
    for (char *p = buf; *p; p++) {
        if ((*p == '/' || *p == '\\') && p != buf && p[-1] != ':') {
            char c = *p;
            *p = '\0';
            MKDIR(buf);
            *p = c;
        }
    }
    if (buf[0]) MKDIR(buf);
}

extern void ac3_card_set_path(int slot, const char *path);
extern const char *ac3_card_path(int slot);

static char card_work[2][640];

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int copy_or_clear(const char *src, const char *dst) {
    FILE *in = src ? fopen(src, "rb") : NULL;
    if (!in) { remove(dst); return 1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out) == 0;
}

static int cards_setup(const ac3_oracle_cfg_t *cfg) {
    int redirect = cfg->play_input || (cfg->out_dir && !(cfg->record_input && cfg->real_cards));
    for (int s = 0; s < 2; s++) {
        const char *real = ac3_card_path(s);
        const char *from = real;
        char snap[640];
        if (cfg->play_input) {
            snprintf(snap, sizeof snap, "%s.card%d.mcd", cfg->play_input, s + 1);
            if (file_exists(snap)) from = snap;
            else if (s == 0)
                printf("oracle: %s has no card snapshot; starting from a copy of %s\n",
                       cfg->play_input, real);
        }
        if (redirect) {
            if (cfg->out_dir) snprintf(card_work[s], sizeof card_work[s], "%s/card%d.mcd", O.dir, s + 1);
            else snprintf(card_work[s], sizeof card_work[s], "%s.run.card%d.mcd", cfg->play_input, s + 1);
            if (!copy_or_clear(from, card_work[s])) {
                fprintf(stderr, "oracle: cannot prepare %s\n", card_work[s]);
                return 0;
            }
            ac3_card_set_path(s, card_work[s]);
        }
        if (cfg->record_input) {
            char out[640];
            snprintf(out, sizeof out, "%s.card%d.mcd", cfg->record_input, s + 1);
            if (!copy_or_clear(ac3_card_path(s), out)) {
                fprintf(stderr, "oracle: cannot snapshot the card to %s\n", out);
                return 0;
            }
        }
    }
    if (redirect) printf("oracle: memory cards redirected to %s (saves/ is not touched)\n", card_work[0]);
    else if (cfg->record_input) printf("oracle: using the real memory cards (%s); snapshot kept with the recording\n",
                                       ac3_card_path(0));
    return 1;
}

static FILE *open_out(const char *name, const char *mode, const char *header) {
    char path[640];
    snprintf(path, sizeof path, "%s/%s", O.dir, name);
    FILE *f = fopen(path, mode);
    if (!f) { fprintf(stderr, "oracle: cannot write %s\n", path); return NULL; }
    if (header) fputs(header, f);
    return f;
}

int ac3_oracle_configure(const ac3_oracle_cfg_t *cfg) {
    memset(&O, 0, sizeof O);
    if (cfg->play_input) {
        O.play = fopen(cfg->play_input, "rb");
        char magic[4];
        u32 hdr[3];
        if (!O.play || fread(magic, 1, 4, O.play) != 4 || memcmp(magic, "AC3I", 4) != 0 ||
            fread(hdr, 4, 3, O.play) != 3 || hdr[0] != 1u || hdr[1] != 24u) {
            fprintf(stderr, "oracle: %s is not an AC3I v1 input recording\n", cfg->play_input);
            return 0;
        }
        O.enabled = 1;
        printf("oracle: controllers from %s\n", cfg->play_input);
    }
    if (cfg->record_input) {
        make_dirs(cfg->record_input, 1);
        O.rec = fopen(cfg->record_input, "wb");
        if (!O.rec) { fprintf(stderr, "oracle: cannot write %s\n", cfg->record_input); return 0; }
        u32 hdr[3] = { 1u, 24u, 0u };
        fwrite("AC3I", 1, 4, O.rec);
        fwrite(hdr, 4, 3, O.rec);
        O.enabled = 1;
        printf("oracle: recording controllers to %s\n", cfg->record_input);
    }
    if (cfg->out_dir) {
        snprintf(O.dir, sizeof O.dir, "%s", cfg->out_dir);
        make_dirs(O.dir, 0);
        O.blocks = cfg->blocks;
        O.ram_every = cfg->ram_every;
        O.frames = open_out("frames.tsv", "w",
            "vblank\tgroup\tphase\twanted\tresident\tmission\tram\tvram\tuploads\tcopies\tstores\tfills\n");
        O.ticks = open_out("ticks.tsv", "w",
            "tick\tvblank\tfn\twhen\tscene\tworld\tentities\tcamera\trand_state\n");
        O.cd = open_out("loads_cd.tsv", "w", "vblank\tlba\tsectors\tdst\tbytes\twhere\n");
        O.vram = open_out("loads_vram.tsv", "w", "vblank\top\tx\ty\tw\th\tsrc\thash\n");
        if (O.blocks) {
            O.ticks_bin = open_out("ticks.bin", "wb", NULL);
            if (O.ticks_bin) { u32 v = 1; fwrite("AC3T", 1, 4, O.ticks_bin); fwrite(&v, 4, 1, O.ticks_bin); }
        }
        if (cfg->frames_fields) {
            char *end;
            O.frames_from = (u32)strtoul(cfg->frames_fields, &end, 10);
            O.frames_to = *end == '-' ? (u32)strtoul(end + 1, &end, 10) : O.frames_from;
            if (*end || !O.frames_to || O.frames_to < O.frames_from) {
                fprintf(stderr, "oracle: --oracle-frames wants A-B (fields)\n");
                return 0;
            }
        }
        if (cfg->state_ticks) {
            char *end;
            O.state_from = (u32)strtoul(cfg->state_ticks, &end, 10);
            O.state_to = *end == '-' ? (u32)strtoul(end + 1, &end, 10) : O.state_from;
            O.state_every = *end == '/' ? (u32)strtoul(end + 1, &end, 10) : 1u;
            if (*end || !O.state_to || O.state_to < O.state_from || !O.state_every) {
                fprintf(stderr, "oracle: --oracle-state wants A-B or A-B/N (ticks, from 1)\n");
                return 0;
            }
        }
        if (cfg->state_ticks || cfg->frames_fields) {
            O.state = open_out("state.bin", "wb", NULL);
            if (!O.state) return 0;
            u32 v = 1;
            fwrite("AC3S", 1, 4, O.state);
            fwrite(&v, 4, 1, O.state);
        }
        O.rand_callers = cfg->rand_callers;
        O.machine_tick = cfg->machine_tick;
        O.machine_field = cfg->machine_field;
        O.machine_phases = cfg->machine_phases;
        if (!O.frames || !O.ticks || !O.cd || !O.vram || (O.blocks && !O.ticks_bin)) return 0;
        O.traced = 1;
        O.enabled = 1;
        printf("oracle: tracing to %s\n", O.dir);
    }
#ifdef AC3_ENHANCED
    if (O.enabled)
        fprintf(stderr, "oracle: warning -- the enhanced build changes GTE results the game "
                        "can see; use ac3.exe as the reference\n");
#endif
    return O.enabled ? cards_setup(cfg) : 1;
}

static void cd_flush(void);

void ac3_oracle_finish(void) {
    cd_flush();
    if (O.rand_callers) {
        FILE *f = open_out("rand_draw.tsv", "w", "ra\tcalls\n");
        if (f) {
            fprintf(f, "# %u frames drawn\n", O.draw_frames);
            for (int i = 0; i < 64 && O.rand_n[i]; i++) fprintf(f, "%08X\t%u\n", O.rand_ra[i], O.rand_n[i]);
            fclose(f);
        }
    }
    FILE **all[] = { &O.frames, &O.ticks, &O.ticks_bin, &O.cd, &O.vram, &O.rec, &O.play, &O.state };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
        if (*all[i]) { fclose(*all[i]); *all[i] = NULL; }
    if (O.traced) printf("oracle: %u simulation ticks traced to %s\n", O.tick, O.dir);
}

int ac3_oracle_deterministic(void) { return O.enabled; }

static void pad_pack(u8 *p, const ac3_pad_t *pad) {
    p[0] = (u8)(pad->connected != 0);
    p[1] = pad->type;
    p[2] = (u8)pad->buttons;
    p[3] = (u8)(pad->buttons >> 8);
    p[4] = pad->lx; p[5] = pad->ly; p[6] = pad->rx; p[7] = pad->ry;
    p[8] = p[9] = 0;
}

static void pad_unpack(const u8 *p, ac3_pad_t *pad) {
    pad->connected = p[0];
    pad->type = p[1];
    pad->buttons = (u16)(p[2] | (p[3] << 8));
    pad->lx = p[4]; pad->ly = p[5]; pad->rx = p[6]; pad->ry = p[7];
}

void ac3_oracle_pads(void) {
    u32 vb = ac3_vblank_count();
    if (O.play && !O.play_done) {
        u8 r[24];
        if (fread(r, 1, sizeof r, O.play) == sizeof r) {
            u32 rvb = (u32)r[0] | ((u32)r[1] << 8) | ((u32)r[2] << 16) | ((u32)r[3] << 24);
            if (rvb != vb && !O.play_warned) {
                fprintf(stderr, "oracle: input recording is for field %u, this is field %u; "
                                "playing it in order anyway\n", rvb, vb);
                O.play_warned = 1;
            }
            pad_unpack(r + 4, &ac3_pad[0]);
            pad_unpack(r + 14, &ac3_pad[1]);
        } else {
            O.play_done = 1;
            for (int i = 0; i < 2; i++) {
                ac3_pad[i].buttons = 0;
                ac3_pad[i].lx = ac3_pad[i].ly = ac3_pad[i].rx = ac3_pad[i].ry = 0x80;
            }
            printf("oracle: input recording ended at field %u\n", vb);
        }
    }
    if (O.rec) {
        u8 r[24];
        r[0] = (u8)vb; r[1] = (u8)(vb >> 8); r[2] = (u8)(vb >> 16); r[3] = (u8)(vb >> 24);
        pad_pack(r + 4, &ac3_pad[0]);
        pad_pack(r + 14, &ac3_pad[1]);
        fwrite(r, 1, sizeof r, O.rec);
        if (vb % 60u == 0) fflush(O.rec);
    }
}

void ac3_oracle_vblank(ctx_t *ctx) {
    (void)ctx;
    if (!O.traced) return;
    u32 scene = guest_word_ptr(GAME_GP + 748);
    s32 group = -1, phase = -1, wanted = -1, resident = -1, mission = -1;
    if (scene) {
        group = (s32)MEM_R32(scene + 44);
        phase = (s32)MEM_R32(scene + 48);
        wanted = (s32)MEM_R32(scene + 52);
        resident = (s32)MEM_R32(scene + 56);
        mission = MEM_R16S(scene + 908);
    }
    u64 hr = hash_bytes(ac3_ram, AC3_RAM_SIZE);
    u64 hv = ac3_vram ? hash_bytes(ac3_vram, (size_t)GPU_VRAM_W * GPU_VRAM_H * 2) : 0;
    fprintf(O.frames, "%u\t%d\t%d\t%d\t%d\t%d\t%016llx\t%016llx\t%u\t%u\t%u\t%u\n",
            ac3_vblank_count(), group, phase, wanted, resident, mission,
            (unsigned long long)hr, (unsigned long long)hv,
            O.n_upload, O.n_copy, O.n_store, O.n_fill);
    O.n_upload = O.n_copy = O.n_store = O.n_fill = 0;
    if (ac3_vblank_count() % 300u == 0) {
        FILE *all[] = { O.frames, O.ticks, O.cd, O.vram, O.ticks_bin, O.state };
        for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
            if (all[i]) fflush(all[i]);
    }
}

int ac3_oracle_is_tick(u32 target) {
    if (!O.traced) return 0;
    for (unsigned i = 0; i < sizeof sim_fns / sizeof sim_fns[0]; i++)
        if (target == sim_fns[i]) return 1;
    return 0;
}

static u32 entity_size(int i) { return i < 2 ? 88u : i < 32 ? 92u : 148u; }

static void block_out(u32 id, u32 addr, u32 len) {
    const void *p = addr ? guest_ptr(addr, len) : NULL;
    u32 hdr[3] = { id, addr, p ? len : 0u };
    fwrite(hdr, 4, 3, O.ticks_bin);
    if (p) fwrite(p, 1, len, O.ticks_bin);
}

void ac3_oracle_tick(ctx_t *ctx, u32 target, int after) {
    (void)ctx;
    if (!after) O.tick++;
    u32 scene = guest_word_ptr(GAME_GP + 748);
    u32 world = guest_word_ptr(GAME_GP + 736);
    u32 slots = guest_word_ptr(GAME_GP + 744);
    u32 cam = guest_word_ptr(CAM_STATE_PTR);

    u64 he = 0xCBF29CE484222325ull;
    u32 ent[64];
    for (int i = 0; i < 64; i++) {
        ent[i] = slots ? guest_word_ptr(slots + 4u * (u32)i) : 0;
        he ^= ent[i] ? hash_guest(ent[i], entity_size(i)) : 0;
        he *= 0x100000001B3ull;
    }
    fprintf(O.ticks, "%u\t%u\t%08X\t%s\t%016llx\t%016llx\t%016llx\t%016llx\t%08X\n",
            O.tick, ac3_vblank_count(), target, after ? "post" : "pre",
            (unsigned long long)(scene ? hash_guest(scene, SCENE_BYTES) : 0),
            (unsigned long long)(world ? hash_guest(world, WORLD_BYTES) : 0),
            (unsigned long long)he,
            (unsigned long long)(cam ? hash_guest(cam, CAMERA_BYTES) : 0),
            MEM_R32(RAND_SEED));

    if (O.ticks_bin) {
        u32 rec[5] = { O.tick, ac3_vblank_count(), target, (u32)after, 4u + 64u };
        fwrite(rec, 4, 5, O.ticks_bin);
        block_out(1, scene, SCENE_BYTES);
        block_out(2, world, WORLD_BYTES);
        block_out(3, RAND_SEED, 4);
        block_out(4, cam, CAMERA_BYTES);
        for (int i = 0; i < 64; i++) block_out(0x100u + (u32)i, ent[i], entity_size(i));
    }
    if (after && O.ram_every && O.tick % O.ram_every == 0) {
        char name[64];
        snprintf(name, sizeof name, "ram_%08u.bin", O.tick);
        FILE *f = open_out(name, "wb", NULL);
        if (f) { fwrite(ac3_ram, 1, AC3_RAM_SIZE, f); fclose(f); }
    }
}

static int state_wanted(void) {
    return O.state && O.tick >= O.state_from && O.tick <= O.state_to &&
           (O.tick - O.state_from) % O.state_every == 0;
}

static void state_snap(ctx_t *ctx, u32 target, u32 checkpoint) {
    static u32 prev[AC3_RAM_SIZE / 4];
    static u32 pairs[2 * (AC3_RAM_SIZE / 4)];
    const u32 *ram = (const u32 *)ac3_ram;
    u32 n = 0;
    for (u32 i = 0; i < AC3_RAM_SIZE / 4; i++)
        if (ram[i] != prev[i]) {
            pairs[2 * n] = i * 4u;
            pairs[2 * n + 1] = ram[i];
            prev[i] = ram[i];
            n++;
        }
    u32 head[5] = { O.tick, ac3_vblank_count(), target, checkpoint, n };
    fwrite(head, 4, 5, O.state);
    fwrite(pairs, 8, n, O.state);
    fwrite(ac3_scratch, 1, 1024, O.state);
    u32 gte[64];
    for (u32 r = 0; r < 32; r++) gte[r] = ac3_gte_mfc2(ctx, r);
    for (u32 r = 0; r < 32; r++) gte[32 + r] = ac3_gte_cfc2(ctx, r);
    fwrite(gte, 4, 64, O.state);
    u32 regs[34];
    memcpy(regs, ctx->r, 32 * 4);
    regs[32] = ctx->hi;
    regs[33] = ctx->lo;
    fwrite(regs, 4, 34, O.state);
}

static void call(ctx_t *ctx, u32 addr, u32 ret) {
    ctx->r[31] = ret;
    ac3_lookup(addr)(ctx);
}

static void run_p7_stepped(ctx_t *ctx) {
    static const u32 steps[8] = { 0x800729DCu, 0x80045898u, 0x80038A3Cu, 0x800517F4u,
                                  0x8004658Cu, 0x8006C5B4u, 0x8007DAC8u, 0x800850D8u };
    u32 *r = ctx->r;
    r[2] = MEM_R32(r[28] + 0x2ECu);
    r[29] -= 0x30u;
    MEM_W32(r[29] + 0x2Cu, r[31]);
    MEM_W32(r[29] + 0x28u, r[16]);
    r[2] = MEM_R32(r[2]);
    r[3] = 0x20000000u;
    r[2] = ~r[2] & r[3];
    if (r[2]) {
        r[2] = MEM_R32(r[28] + 0x2E0u);
        r[16] = MEM_R32(r[2] + 0x28u);
        for (u32 k = 0; k < 8; k++) {
            call(ctx, steps[k], 0x80032F78u + 8u * k);
            state_snap(ctx, 0x80032F3Cu, k + 1u);
        }
        r[3] = MEM_R32(CAM_STATE_PTR);
        r[2] = MEM_R32(r[3] + 0x1Cu);
        MEM_W32(r[29] + 0x10u, r[2]);
        r[2] = MEM_R32(r[3] + 0x08u);
        MEM_W32(r[29] + 0x14u, r[2]);
        r[2] = MEM_R32(r[3] + 0x0Cu);
        MEM_W32(r[29] + 0x18u, r[2]);
        r[2] = MEM_R32(r[3] + 0x10u);
        r[4] = r[29] + 0x10u;
        MEM_W32(r[29] + 0x1Cu, r[2]);
        call(ctx, 0x8003A23Cu, 0x80032FF0u);
        state_snap(ctx, 0x80032F3Cu, 9u);
        r[2] = MEM_R32(CAM_STATE_PTR);
        r[4] = MEM_R32(r[29] + 0x24u);
        r[2] = MEM_R32(r[2] + 0x18u);
        if (r[2] != r[4]) {
            r[2] = MEM_R32(r[28] + 0x2E0u);
            r[2] = MEM_R32(r[2]) & 2u;
            if (r[2]) call(ctx, 0x8007E890u, 0x80033030u);
        }
        r[2] = MEM_R32(r[29] + 0x20u);
        r[4] = MEM_R32(CAM_STATE_PTR);
        r[3] = MEM_R32(r[28] + 0x2E0u);
        MEM_W32(r[16] + 0xBCu, r[2]);
        r[2] = MEM_R32(r[29] + 0x24u);
        MEM_W32(r[4] + 0x18u, r[2]);
        r[2] = MEM_R32(r[3]) & 2u;
        if (r[2]) call(ctx, 0x8007F0F8u, 0x8003306Cu);
    }
    r[31] = MEM_R32(r[29] + 0x2Cu);
    r[16] = MEM_R32(r[29] + 0x28u);
    r[29] += 0x30u;
}

int ac3_oracle_steps_frame(u32 target) {
    return (O.state || O.rand_callers || O.machine_tick || O.machine_field || O.machine_phases) &&
           (target == 0x80033EF0u || target == 0x80034000u || target == 0x80034040u ||
            target == 0x80034160u || target == 0x800342ACu || target == 0x800342ECu);
}

void ac3_ms_u32(FILE *f, u32 v) {
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    fwrite(b, 1, 4, f);
}

void ac3_ms_bytes(FILE *f, const void *p, u32 n) {
    ac3_ms_u32(f, n);
    if (n) fwrite(p, 1, n, f);
}

void ac3_ms_tag(FILE *f, const char *tag) {
    char t[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4 && tag[i]; i++) t[i] = tag[i];
    fwrite(t, 1, 4, f);
}

static void machine_dump(const char *name, u32 checkpoint) {
    FILE *f = open_out(name, "wb", NULL);
    if (!f) return;
    fwrite("AC3M", 1, 4, f);
    ac3_ms_u32(f, 2);
    ac3_ms_u32(f, O.tick);
    ac3_ms_u32(f, ac3_vblank_count());
    ac3_ms_u32(f, checkpoint);
    ac3_etc_machine_save(f);
    ac3_api_machine_save(f);
    ac3_spu_machine_save(f);
    ac3_spu_core_machine_save(f);
    ac3_xa_machine_save(f);
    ac3_cd_machine_save(f);
    ac3_pad_machine_save(f);
    ac3_pad2_machine_save(f);
    ac3_gpu_machine_save(f);
    ac3_gpu_sw_machine_save(f);
    ac3_ms_tag(f, "PLAY");
    ac3_ms_u32(f, O.play ? (u32)ftell(O.play) : 0xFFFFFFFFu);
    ac3_ms_u32(f, (u32)O.play_done);
    ac3_ms_tag(f, "END");
    fclose(f);
}

int ac3_oracle_phase_call(u32 ra) {
    return (ra == 0x800263A4u || ra == 0x800263D0u) &&
           ((O.state && O.frames_to) || (O.machine_field && !O.machine_done) || O.machine_phases);
}

void ac3_oracle_phase_snap(ctx_t *ctx, u32 target, u32 checkpoint) {
    u32 vb = ac3_vblank_count();
    if (checkpoint == 30u && O.machine_field && !O.machine_done && vb >= O.machine_field && O.dir[0]) {
        machine_dump("machine.bin", 30u);
        O.machine_done = 1;
        printf("oracle: machine state at field %u written\n", vb);
    }
    if (checkpoint == 30u && O.machine_phases && target != O.last_phase && O.dir[0]) {
        char name[64];
        snprintf(name, sizeof name, "machine_%05u.bin", vb);
        machine_dump(name, 30u);
    }
    if (checkpoint == 30u) O.last_phase = target;
    if (O.state && O.frames_to && vb >= O.frames_from && vb <= O.frames_to) state_snap(ctx, target, checkpoint);
}

void ac3_oracle_frame_snap(ctx_t *ctx, u32 target, u32 checkpoint) {
    if (O.rand_callers) {
        if (checkpoint == 18u) { O.in_draw = 1; O.draw_frames++; }
        else if (checkpoint == 19u) O.in_draw = 0;
    }
    if (state_wanted()) state_snap(ctx, target, checkpoint);
    if (checkpoint == 20u && O.machine_tick && O.tick == O.machine_tick && !O.machine_done && O.dir[0]) {
        machine_dump("machine.bin", 20u);
        O.machine_done = 1;
        printf("oracle: machine state at tick %u written\n", O.tick);
    }
}

int ac3_oracle_in_draw(void) { return O.in_draw; }

void ac3_oracle_rand_caller(u32 ra) {
    for (int i = 0; i < 64; i++) {
        if (O.rand_n[i] && O.rand_ra[i] != ra) continue;
        O.rand_ra[i] = ra;
        O.rand_n[i]++;
        return;
    }
}

void ac3_oracle_run_tick(ctx_t *ctx, u32 target, ac3_func_t f) {
    if (!state_wanted()) { f(ctx); return; }
    state_snap(ctx, target, 0u);
    if (target == 0x80032F3Cu) run_p7_stepped(ctx);
    else f(ctx);
    state_snap(ctx, target, 15u);
}

static void cd_flush(void) {
    if (!O.run_open || !O.cd) { O.run_open = 0; return; }
    char where[96];
    ac3_cd_describe_lba(O.run_lba0, where, sizeof where);
    fprintf(O.cd, "%u\t%u\t%u\t%08X\t%u\t%s\n", O.run_vblank, O.run_lba0,
            O.run_lba - O.run_lba0 + 1u, O.run_dst0, O.run_bytes, where);
    O.run_open = 0;
}

void ac3_oracle_cd(u32 lba, u32 dst, u32 bytes) {
    if (!O.traced) return;
    if (O.run_open && lba == O.run_lba + 1u && dst == O.run_dst) {
        O.run_lba = lba;
        O.run_dst += bytes;
        O.run_bytes += bytes;
        return;
    }
    cd_flush();
    O.run_open = 1;
    O.run_vblank = ac3_vblank_count();
    O.run_lba0 = O.run_lba = lba;
    O.run_dst0 = dst;
    O.run_dst = dst + bytes;
    O.run_bytes = bytes;
}

#define SEEN_SLOTS 8192u
static struct { u64 key, hash; } seen[SEEN_SLOTS];

static int seen_before(int op, const PSX_RECT *r, u32 src, u64 h) {
    u64 key = ((u64)(u32)op << 56) ^ ((u64)(u16)r->x << 40) ^ ((u64)(u16)r->y << 28) ^
              ((u64)(u16)r->w << 16) ^ (u64)(u16)r->h ^ ((u64)src << 20);
    u32 i = (u32)(key * 0x9E3779B97F4A7C15ull >> 51) & (SEEN_SLOTS - 1u);
    for (u32 probe = 0; probe < 16; probe++, i = (i + 1u) & (SEEN_SLOTS - 1u)) {
        if (seen[i].key == key) {
            if (seen[i].hash == h) return 1;
            seen[i].hash = h;
            return 0;
        }
        if (!seen[i].key) { seen[i].key = key; seen[i].hash = h; return 0; }
    }
    return 0;
}

void ac3_oracle_vram(int op, const PSX_RECT *r, u32 src, s32 dx, s32 dy) {
    if (!O.traced) return;
    static const char *names[] = { "upload", "upload-ot", "copy", "store", "fill" };
    switch (op) {
    case AC3_OVRAM_UPLOAD: case AC3_OVRAM_UPLOAD_OT: O.n_upload++; break;
    case AC3_OVRAM_COPY:  O.n_copy++; break;
    case AC3_OVRAM_STORE: O.n_store++; break;
    default:              O.n_fill++; return;
    }
    if ((s32)r->w * r->h < 16) return;
    PSX_RECT at = *r;
    if (op == AC3_OVRAM_COPY) { at.x = (s16)dx; at.y = (s16)dy; }
    u64 h = hash_vram_rect(&at);
    u32 key_src = op == AC3_OVRAM_COPY ? ((u32)(u16)r->x | ((u32)(u16)r->y << 16)) : src;
    if (seen_before(op, &at, key_src, h)) return;
    char from[24];
    if (op == AC3_OVRAM_COPY) snprintf(from, sizeof from, "%d,%d", r->x, r->y);
    else snprintf(from, sizeof from, "%08X", src);
    fprintf(O.vram, "%u\t%s\t%d\t%d\t%d\t%d\t%s\t%016llx\n", ac3_vblank_count(), names[op],
            at.x, at.y, r->w, r->h, from, (unsigned long long)h);
}
