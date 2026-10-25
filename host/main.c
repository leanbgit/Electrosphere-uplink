#include "ac3_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ac3_mem_init(void);
extern const struct { u32 addr; ac3_func_t fn; } ac3_func_table[];
extern const unsigned ac3_func_table_len;

#define PSX_EXE_HEADER 0x800

static int load_exe(const char *path, u32 *entry) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }

    u8 head[PSX_EXE_HEADER];
    if (fread(head, 1, sizeof head, f) != sizeof head ||
        memcmp(head, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a PS-EXE\n", path);
        fclose(f);
        return 0;
    }
    u32 pc0, t_addr, t_size;
    memcpy(&pc0,    head + 0x10, 4);
    memcpy(&t_addr, head + 0x18, 4);
    memcpy(&t_size, head + 0x1C, 4);

    u32 phys = ac3_phys(t_addr);
    if (phys + t_size > AC3_RAM_SIZE) {
        fprintf(stderr, "%s: text 0x%08X+%u does not fit in RAM\n",
                path, t_addr, t_size);
        fclose(f);
        return 0;
    }
    size_t got = fread(ac3_ram + phys, 1, t_size, f);
    fclose(f);
    if (got != t_size) {
        fprintf(stderr, "%s: short read (%zu of %u)\n", path, got, t_size);
        return 0;
    }
    printf("loaded %s\n", path);
    printf("  text  0x%08X .. 0x%08X (%u bytes)\n",
           t_addr, t_addr + t_size, t_size);
    printf("  entry 0x%08X\n", pc0);
    *entry = pc0;
    return 1;
}

extern int ac3_selftest_gpu(void);
extern int ac3_selftest_raster(void);
extern int ac3_selftest_cd(const char *image, const char *image2);
extern int ac3_selftest_english(const char *image, const char *image2);
extern int ac3_selftest_libc(void);
extern int ac3_selftest_gte(void);
extern int ac3_selftest_card(void);
extern int ac3_selftest_spu(const char *disc);
extern int ac3_selftest_libgpu(const char *exe);
extern void ac3_boot_no_audio(void);
extern void ac3_boot_audio_dump(const char *path);
extern void ac3_boot_second_disc(const char *path);
extern void ac3_boot_one_disc(void);
extern int ac3_boot(const char *exe, const char *disc, u32 frames, int trace,
                    int auto_start, int windowed);
extern void ac3_boot_set_dump(u32 addr, u32 len);
extern void ac3_boot_dump_vram(void);
extern void ac3_boot_screenshot_at(u32 frame);
extern void ac3_boot_screenshot_every(u32 n);
extern void ac3_boot_capture_at(u32 frame);
extern void ac3_boot_capture_every(u32 n);
extern void ac3_capture_on_video(void);
extern u32 ac3_watch_lo, ac3_watch_hi;
extern int ac3_cd_rate;
extern const char *ac3_mdec_dump;
#include "ac3_oracle.h"
#include "ac3_settings.h"
#ifdef AC3_ENHANCED
#include "hw/hw_glue.h"
#endif

int main(int argc, char **argv) {
    const char *exe = (argc > 1) ? argv[1] : "game/ACE3.BIN";
    ac3_mem_init();

    ac3_settings_load(AC3_SETTINGS_FILE);
    const char *lang = ac3_settings_get("language");
    ac3_lang_set_english(lang && !strcmp(lang, "en"));
    ac3_lang_set_patch_dir(ac3_settings_get("english_patch_dir"));
    int lang_cli = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--english") == 0) { ac3_lang_set_english(1); lang_cli = 1; }
        else if (strcmp(argv[i], "--japanese") == 0) { ac3_lang_set_english(0); lang_cli = 1; }
        else if (strcmp(argv[i], "--english-patch") == 0 && i + 1 < argc)
            ac3_lang_set_patch_dir(argv[i + 1]);
    }

#ifdef AC3_ENHANCED
    hw_settings_load();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            ac3_hd.scale = atoi(argv[i + 1]);
            if (ac3_hd.scale < 1) ac3_hd.scale = 1;
            if (ac3_hd.scale > HW_MAX_SCALE) ac3_hd.scale = HW_MAX_SCALE;
        }
        else if (strcmp(argv[i], "--widescreen") == 0)     ac3_hd.widescreen = 1;
        else if (strcmp(argv[i], "--no-widescreen") == 0)  ac3_hd.widescreen = 0;
        else if (strcmp(argv[i], "--no-perspective") == 0) ac3_hd.perspective = 0;
        else if (strcmp(argv[i], "--no-pgxp") == 0)        ac3_hd.pgxp = 0;
        else if (strcmp(argv[i], "--no-fx") == 0)          ac3_hd.fx = 0;
        else if (strcmp(argv[i], "--no-far") == 0)         ac3_hd.far_terrain = 0;
        else if (strcmp(argv[i], "--no-ocean") == 0)       ac3_hd.ocean = 0;
        else if (strcmp(argv[i], "--no-clouds") == 0)      ac3_hd.clouds = 0;
        else if (strcmp(argv[i], "--no-forest") == 0)      ac3_hd.forest = 0;
        else if (strcmp(argv[i], "--no-atmosphere") == 0)  ac3_hd.atmosphere = 0;
        else if (strcmp(argv[i], "--no-gi") == 0)          ac3_hd.gi = 0;
        else if (strcmp(argv[i], "--no-sun") == 0)         ac3_hd.sun = 0;
        else if (strcmp(argv[i], "--no-game-fog") == 0)    ac3_hd.game_fog = 0;
        else if (strcmp(argv[i], "--glue-trace") == 0 && i + 1 < argc) {
            extern u32 hw_glue_trace_field;
            hw_glue_trace_field = (u32)atoi(argv[i + 1]);
        }
    }
#ifndef AC3_HW_WEBGPU  /* native-only tools: Vulkan self-test and frame replay */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest-hw") == 0)
            return ac3_selftest_hw();
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            const char *out = "replay", *ram = NULL;
            for (int j = 1; j + 1 < argc; j++) {
                if (strcmp(argv[j], "--out") == 0) out = argv[j + 1];
                if (strcmp(argv[j], "--ram") == 0) ram = argv[j + 1];
                if (strcmp(argv[j], "--sun-test") == 0 && j + 2 < argc) {
                    extern float hw_sun_test[2];
                    extern int hw_sun_test_on;
                    hw_sun_test[0] = (float)atof(argv[j + 1]);
                    hw_sun_test[1] = (float)atof(argv[j + 2]);
                    hw_sun_test_on = 1;
                }
                if (strcmp(argv[j], "--cam-test") == 0 && j + 5 < argc) {
                    extern float hw_cam_test[5];
                    extern int hw_cam_test_on;
                    for (int k = 0; k < 5; k++) hw_cam_test[k] = (float)atof(argv[j + 1 + k]);
                    hw_cam_test_on = 1;
                }
                if (strcmp(argv[j], "--cam-sweep") == 0 && j + 6 < argc) {
                    extern int hw_cam_sweep_n;
                    extern float hw_cam_sweep_d[5];
                    hw_cam_sweep_n = atoi(argv[j + 1]);
                    if (hw_cam_sweep_n < 1) hw_cam_sweep_n = 1;
                    for (int k = 0; k < 5; k++) hw_cam_sweep_d[k] = (float)atof(argv[j + 2 + k]);
                }
            }
            return hw_replay_main(argv[i + 1], out, ram);
        }
    }
#endif /* !AC3_HW_WEBGPU */
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest-gpu") == 0)
            return ac3_selftest_gpu();
        if (strcmp(argv[i], "--selftest-raster") == 0)
            return ac3_selftest_raster();
        if (strcmp(argv[i], "--selftest-libgpu") == 0)
            return ac3_selftest_libgpu(i + 1 < argc ? argv[i + 1] : "");
        if (strcmp(argv[i], "--selftest-cd") == 0)
            return ac3_selftest_cd(i + 1 < argc ? argv[i + 1] : "",
                                   i + 2 < argc ? argv[i + 2] : NULL);
        if (strcmp(argv[i], "--selftest-english") == 0) {
            ac3_lang_set_english(1);
            return ac3_selftest_english(i + 1 < argc ? argv[i + 1] : "",
                                        i + 2 < argc ? argv[i + 2] : NULL);
        }
        if (strcmp(argv[i], "--selftest-libc") == 0)
            return ac3_selftest_libc();
        if (strcmp(argv[i], "--selftest-gte") == 0)
            return ac3_selftest_gte();
        if (strcmp(argv[i], "--selftest-card") == 0)
            return ac3_selftest_card();
        if (strcmp(argv[i], "--selftest-spu") == 0)
            return ac3_selftest_spu(i + 1 < argc ? argv[i + 1] : "");
        if (strcmp(argv[i], "--boot") == 0) {
            const char *disc = NULL;
            u32 frames = 60;
            int trace = 0, autostart = 0, windowed = 0;
            const char *img = "game/ACE3.BIN";
            ac3_oracle_cfg_t oracle = { 0 };
            for (int j = 1; j < argc; j++) {
                if (strcmp(argv[j], "--disc") == 0 && j + 1 < argc) disc = argv[++j];
                else if (strcmp(argv[j], "--record-input") == 0 && j + 1 < argc)
                    oracle.record_input = argv[++j];
                else if (strcmp(argv[j], "--play-input") == 0 && j + 1 < argc)
                    oracle.play_input = argv[++j];
                else if (strcmp(argv[j], "--oracle-out") == 0 && j + 1 < argc)
                    oracle.out_dir = argv[++j];
                else if (strcmp(argv[j], "--oracle-blocks") == 0) oracle.blocks = 1;
                else if (strcmp(argv[j], "--oracle-ram-every") == 0 && j + 1 < argc)
                    oracle.ram_every = (u32)strtoul(argv[++j], NULL, 0);
                else if (strcmp(argv[j], "--oracle-real-cards") == 0) oracle.real_cards = 1;
                else if (strcmp(argv[j], "--oracle-state") == 0 && j + 1 < argc)
                    oracle.state_ticks = argv[++j];
                else if (strcmp(argv[j], "--oracle-rand-callers") == 0) oracle.rand_callers = 1;
                else if (strcmp(argv[j], "--oracle-machine") == 0 && j + 1 < argc)
                    oracle.machine_tick = (u32)strtoul(argv[++j], NULL, 0);
                else if (strcmp(argv[j], "--oracle-machine-field") == 0 && j + 1 < argc)
                    oracle.machine_field = (u32)strtoul(argv[++j], NULL, 0);
                else if (strcmp(argv[j], "--oracle-frames") == 0 && j + 1 < argc)
                    oracle.frames_fields = argv[++j];
                else if (strcmp(argv[j], "--oracle-machine-phases") == 0) oracle.machine_phases = 1;
                else if (strcmp(argv[j], "--cards") == 0 && j + 1 < argc) {
                    extern void ac3_card_set_path(int slot, const char *path);
                    static char card_paths[2][600];
                    const char *dir = argv[++j];
                    for (int s = 0; s < 2; s++) {
                        snprintf(card_paths[s], sizeof card_paths[s], "%s/card%d.mcd", dir, s + 1);
                        ac3_card_set_path(s, card_paths[s]);
                    }
                }
                else if (strcmp(argv[j], "--disc2") == 0 && j + 1 < argc)
                    ac3_boot_second_disc(argv[++j]);
                else if (strcmp(argv[j], "--one-disc") == 0) ac3_boot_one_disc();
                else if (strcmp(argv[j], "--frames") == 0 && j + 1 < argc)
                    frames = (u32)atoi(argv[++j]);
                else if (strcmp(argv[j], "--trace") == 0) trace = 1;
                else if (strcmp(argv[j], "--exe") == 0 && j + 1 < argc) img = argv[++j];
                else if (strcmp(argv[j], "--autostart") == 0) autostart = 1;
                else if (strcmp(argv[j], "--window") == 0) windowed = 1;
                else if (strcmp(argv[j], "--no-audio") == 0) ac3_boot_no_audio();
                else if (strcmp(argv[j], "--audio-dump") == 0 && j + 1 < argc)
                    ac3_boot_audio_dump(argv[++j]);
                else if (strcmp(argv[j], "--cd-rate") == 0 && j + 1 < argc)
                    ac3_cd_rate = atoi(argv[++j]);
                else if (strcmp(argv[j], "--dump-frames") == 0 && j + 1 < argc)
                    ac3_mdec_dump = argv[++j];
                else if (strcmp(argv[j], "--watch") == 0 && j + 2 < argc) {
                    ac3_watch_lo = (u32)strtoul(argv[j + 1], NULL, 0);
                    ac3_watch_hi = ac3_watch_lo +
                                   (u32)strtoul(argv[j + 2], NULL, 0);
                    j += 2;
                }
                else if (strcmp(argv[j], "--dump-vram") == 0) ac3_boot_dump_vram();
                else if (strcmp(argv[j], "--screenshot") == 0 && j + 1 < argc)
                    ac3_boot_screenshot_at((u32)strtoul(argv[++j], NULL, 0));
                else if (strcmp(argv[j], "--screenshot-every") == 0 && j + 1 < argc)
                    ac3_boot_screenshot_every((u32)strtoul(argv[++j], NULL, 0));
                else if (strcmp(argv[j], "--capture-at") == 0 && j + 1 < argc)
                    ac3_boot_capture_at((u32)strtoul(argv[++j], NULL, 0));
                else if (strcmp(argv[j], "--capture-every") == 0 && j + 1 < argc)
                    ac3_boot_capture_every((u32)strtoul(argv[++j], NULL, 0));
                else if (strcmp(argv[j], "--capture-on-video") == 0)
                    ac3_capture_on_video();
                else if (strcmp(argv[j], "--dump") == 0 && j + 2 < argc) {
                    u32 a = (u32)strtoul(argv[j + 1], NULL, 0);
                    u32 l = (u32)strtoul(argv[j + 2], NULL, 0);
                    ac3_boot_set_dump(a, l);
                    j += 2;
                }
            }
            if (!lang_cli && (oracle.record_input || oracle.play_input || oracle.out_dir))
                ac3_lang_set_english(0);
            if (!ac3_oracle_configure(&oracle)) return 1;
            int rc = ac3_boot(img, disc, frames, trace, autostart, windowed);
#ifdef AC3_ENHANCED
            extern int ac3_render_restart_pending(int *english);
            extern int ac3_render_relaunch(int argc, char **argv, int english);
            int restart_english = 0;
            if (ac3_render_restart_pending(&restart_english))
                return ac3_render_relaunch(argc, argv, restart_english);
#endif
            return rc;
        }
    }

    u32 entry = 0;
    if (!load_exe(exe, &entry)) return 1;

    printf("\nrecompiled functions: %u\n", ac3_func_table_len);
    ac3_func_t f = ac3_lookup(entry);
    printf("entry 0x%08X resolves to %s\n", entry, f ? "a recompiled function" : "NOTHING");

    unsigned unsorted = 0;
    for (unsigned i = 1; i < ac3_func_table_len; i++)
        if (ac3_func_table[i - 1].addr >= ac3_func_table[i].addr) unsorted++;
    printf("dispatch table sorted: %s (%u violations)\n",
           unsorted ? "NO" : "yes", unsorted);
    return f ? 0 : 1;
}
