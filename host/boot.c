#include "hle.h"
#include "gpu_backend.h"
#include "spu_backend.h"
#include "spu_core.h"
#include "xa_adpcm.h"
#include "ac3_capture.h"
#include "ac3_oracle.h"
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "ac3_input.h"

extern void ac3_mem_init(void);
extern int  ac3_disc_open(const char *path, int number);
extern int  ac3_disc_attach(const char *path);
extern int  ac3_disc_other_path(const char *path, char *out, size_t n);
extern void ac3_gpu_stats_dump(void);
extern void ac3_prof_dump(int top);
extern void ac3_edges_dump(int top);
extern void ac3_watch_dump(void);
extern u32  ac3_vblank_count(void);
extern int  ac3_hle_trace_enabled;

#define PSX_EXE_HEADER 0x800

static jmp_buf boot_exit;
static int     boot_running;
static u32     frame_limit;
static const char *stop_reason = "returned normally";
static int autostart;
static u32 dump_addr, dump_len;
static int dump_vram;

void ac3_boot_dump_vram(void) { dump_vram = 1; }

static u32 shot_frame;
extern void ac3_render_screenshot(const char *path);
extern void ac3_render_screenshot_when_drawing(const char *path, u32 min_verts);
void ac3_boot_screenshot_at(u32 frame) {
    if (frame) shot_frame = frame;
    else ac3_render_screenshot_when_drawing("frame.ppm", 64);
}

static u32 shot_every;
void ac3_boot_screenshot_every(u32 n) { shot_every = n; }

static u32 cap_at, cap_every;
void ac3_boot_capture_at(u32 frame)    { cap_at = frame; }
void ac3_boot_capture_every(u32 n)     { cap_every = n; }

void ac3_boot_set_dump(u32 addr, u32 len) { dump_addr = addr; dump_len = len; }

static void scripted_input(void) {
    if (!autostart) return;
    u32 f = ac3_vblank_count();
    ac3_pad[0].connected = 1;
    u16 b = 0;
    if (f > 120) {
        u32 phase = (f / 15) % 8;
        if (phase == 0) b = (u16)AC3_BTN_START;
        else if (phase == 4) b = (u16)AC3_BTN_CIRCLE;
    }
    ac3_pad[0].buttons = b;
}

#ifdef AC3_COOP_PROBE
#include "hle/coop_probe.h"
#endif
void ac3_boot_frame_hook(void) {
    if (!boot_running) return;
#ifdef AC3_COOP_PROBE
    ac3_coop_probe_field();
#endif
    scripted_input();
    if (shot_frame && ac3_vblank_count() == shot_frame)
        ac3_render_screenshot("frame.ppm");
    if (shot_every && ac3_vblank_count() % shot_every == 0) {
        char path[64];
        snprintf(path, sizeof path, "shot_%05u.ppm", ac3_vblank_count());
        ac3_render_screenshot(path);
    }
    if (cap_at && ac3_vblank_count() == cap_at) ac3_capture_request();
    if (cap_every && ac3_vblank_count() % cap_every == 0) ac3_capture_request();
    if (ac3_render_quit_requested()) {
        stop_reason = "window closed";
        boot_running = 0;
        longjmp(boot_exit, 1);
    }
    if (frame_limit && ac3_vblank_count() >= frame_limit) {
        stop_reason = "frame limit reached";
        boot_running = 0;
        longjmp(boot_exit, 1);
    }
}

static int load_exe(const char *path, u32 *entry, u32 *sp) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    u8 head[PSX_EXE_HEADER];
    if (fread(head, 1, sizeof head, f) != sizeof head ||
        memcmp(head, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a PS-EXE\n", path);
        fclose(f);
        return 0;
    }
    u32 pc0, t_addr, t_size, s_addr;
    memcpy(&pc0,    head + 0x10, 4);
    memcpy(&t_addr, head + 0x18, 4);
    memcpy(&t_size, head + 0x1C, 4);
    memcpy(&s_addr, head + 0x30, 4);

    u32 phys = ac3_phys(t_addr);
    if (phys + t_size > AC3_RAM_SIZE) {
        fprintf(stderr, "%s: does not fit in RAM\n", path);
        fclose(f);
        return 0;
    }
    size_t got = fread(ac3_ram + phys, 1, t_size, f);
    fclose(f);
    if (got != t_size) { fprintf(stderr, "%s: short read\n", path); return 0; }

    printf("loaded %s\n", path);
    printf("  text  0x%08X + %u bytes\n", t_addr, t_size);
    printf("  entry 0x%08X   initial sp 0x%08X\n", pc0, s_addr);
    *entry = pc0;
    *sp = s_addr;
    return 1;
}

extern int ac3_disc_english_active(void);
extern int ac3_disc_other_english(void);
extern int ac3_disc_read_file(int which, const char *name, u8 *buf, u32 cap, u32 *size);
extern const struct { u32 addr; ac3_func_t fn; } ac3_func_table[];
extern const unsigned ac3_func_table_len;

static int english_exe_read(int which, const u8 *head, u8 *out, u32 cap) {
    u32 size = 0, t_size;
    memcpy(&t_size, head + 0x1C, 4);
    return ac3_disc_read_file(which, "ACE3.BIN", out, cap, &size) &&
           size >= PSX_EXE_HEADER + t_size && memcmp(out, "PS-X EXE", 8) == 0 &&
           memcmp(out + 0x10, head + 0x10, 0x24) == 0;
}

static void english_exe(const char *exe) {
    static u8 disc_exe[2][0x100000];
    u8 head[PSX_EXE_HEADER];
    FILE *f = fopen(exe, "rb");
    if (!f) return;
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    if (got != sizeof head) return;
    int have[2];
    have[0] = ac3_disc_english_active() && english_exe_read(0, head, disc_exe[0], sizeof disc_exe[0]);
    have[1] = ac3_disc_other_english() && english_exe_read(1, head, disc_exe[1], sizeof disc_exe[1]);
    if (!have[0] && !have[1]) {
        fprintf(stderr, "english: no ACE3.BIN on the discs matches %s; its data is left as it is\n", exe);
        return;
    }
    u32 t_addr, t_size;
    memcpy(&t_addr, head + 0x18, 4);
    memcpy(&t_size, head + 0x1C, 4);
    u8 *ram = ac3_ram + ac3_phys(t_addr);
    u32 code_lo = ac3_func_table_len ? ac3_func_table[0].addr : 0;
    u32 code_hi = ac3_func_table_len ? ac3_func_table[ac3_func_table_len - 1].addr : 0;
    u32 changed = 0, other = 0, in_code = 0;
    for (u32 i = 0; i < t_size; i++) {
        int d = have[0] && disc_exe[0][PSX_EXE_HEADER + i] != ram[i] ? 0 :
                have[1] && disc_exe[1][PSX_EXE_HEADER + i] != ram[i] ? 1 : -1;
        if (d < 0) continue;
        changed++;
        other += (u32)d;
        if (t_addr + i >= code_lo && t_addr + i <= code_hi) in_code++;
        ram[i] = disc_exe[d][PSX_EXE_HEADER + i];
    }
    printf("english: %u byte%s of the executable taken from the discs' ACE3.BIN",
           changed, changed == 1 ? "" : "s");
    if (other) printf(" (%u from the other disc)", other);
    printf("\n");
    if (in_code)
        fprintf(stderr, "english: %u of them fall in recompiled code, where they have no effect\n",
                in_code);
}

static int audio_off;
void ac3_boot_no_audio(void) { audio_off = 1; }

static const char *audio_dump_path;
void ac3_boot_audio_dump(const char *path) { audio_dump_path = path; }

static const char *second_disc;
static int one_disc;
void ac3_boot_second_disc(const char *path) { second_disc = path; }
void ac3_boot_one_disc(void) { one_disc = 1; }

#ifdef AC3_INTERP_ONLY
static u32 ac3_interp_entry_addr;
static void ac3_interp_entry(ctx_t *ctx) { ac3_interpret(ctx, ac3_interp_entry_addr); }
#endif
int ac3_boot(const char *exe, const char *disc, u32 frames, int trace,
             int auto_start, int windowed) {
    autostart = auto_start;
    ac3_mem_init();
    ac3_vram_init();
#ifdef __EMSCRIPTEN__
    extern const gpu_backend_t gpu_backend_web;
    extern const spu_backend_t spu_backend_web;
    if (windowed) gpu_select_backend(&gpu_backend_web);
    if (windowed && !audio_off) spu_select_backend(&spu_backend_web);
#elif defined(AC3_HEADLESS)
    /* platform/headless: no window, no sound device (co-op research bench) */
    extern const gpu_backend_t gpu_backend_headless;
    extern const spu_backend_t spu_backend_headless;
    if (windowed) gpu_select_backend(&gpu_backend_headless);
    if (windowed && !audio_off) spu_select_backend(&spu_backend_headless);
#else
    if (windowed) gpu_select_backend(&gpu_backend_vulkan);
    if (windowed && !audio_off) spu_select_backend(&spu_backend_sdl);
#endif
    if (audio_dump_path) ac3_spu_dump_open(audio_dump_path);
    ac3_hle_trace_enabled = trace;
    frame_limit = frames;

    u32 entry = 0, sp = 0;
    if (!load_exe(exe, &entry, &sp)) return 1;
    if (disc && *disc) {
        if (!ac3_disc_open(disc, 1)) return 1;
        char other[1024];
        const char *second = second_disc;
        if (!second && ac3_disc_other_path(disc, other, sizeof other)) second = other;
        if (one_disc) second = NULL;
        if (second) ac3_disc_attach(second);
        else if (!one_disc)
            printf("one disc image only: the game will stop at the disc change\n");
        if (ac3_disc_english_active() || ac3_disc_other_english()) english_exe(exe);
    }

    ac3_func_t fn = ac3_lookup(entry);
#ifdef AC3_INTERP_ONLY
    /* No recompiled code in this build: the game's own ACE3.BIN is interpreted. */
    if (!fn) fn = ac3_interp_entry;
    ac3_interp_entry_addr = entry;
#endif
    if (!fn) {
        fprintf(stderr, "entry 0x%08X is not a recompiled function\n", entry);
        return 1;
    }

    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[29] = sp ? sp : 0x801FFFF0u;
    ctx.r[31] = 0;
    ctx.pc = entry;

    printf("\nrunning entry point (%u frame limit)\n", frames);
    printf("----------------------------------------------------------\n");

    boot_running = 1;
    if (setjmp(boot_exit) == 0) {
        fn(&ctx);
        stop_reason = "entry point returned";
    }
    boot_running = 0;
    ac3_oracle_finish();
#ifdef AC3_COOP_PROBE
    ac3_coop_probe_finish();
#endif

    printf("----------------------------------------------------------\n");
    printf("stopped: %s\n", stop_reason);
    printf("frames rendered: %u\n", ac3_vblank_count());
    printf("\n");
    ac3_spu_dump_close();
    ac3_gpu_stats_dump();
    ac3_spu_stats_dump();
    ac3_xa_stats_dump();
    ac3_prof_dump(40);
    ac3_edges_dump(30);
    ac3_watch_dump();

    if (dump_vram) {
        FILE *f = fopen("vram.bin", "wb");
        if (f) {
            fwrite(ac3_vram, 1, GPU_VRAM_W * GPU_VRAM_H * 2, f);
            fclose(f);
            printf("dumped VRAM %dx%d -> vram.bin\n", GPU_VRAM_W, GPU_VRAM_H);
        }
    }

    if (dump_len) {
        FILE *f = fopen("memdump.bin", "wb");
        if (f) {
            void *p = ac3_ram + ac3_phys(dump_addr);
            fwrite(p, 1, dump_len, f);
            fclose(f);
            printf("dumped %u bytes of guest RAM at 0x%08X -> memdump.bin\n",
                   dump_len, dump_addr);
        }
    }
    return 0;
}
