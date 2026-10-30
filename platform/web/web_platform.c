/*
 * web_platform.c -- browser replacement for renderer/gpu_vulkan.c,
 * renderer/ac3_sdl_audio.c and renderer/ac3_sdl_input.c.
 *
 * The whole program runs inside one dedicated Web Worker (see web/worker.js),
 * so it may block. Each emulated field (59.94 Hz):
 *   1. read the controller state the page wrote into a shared buffer
 *   2. convert the visible part of VRAM to RGBA and post it to the page
 *   3. sleep until the next field deadline (same clock as the native build)
 * Audio samples go into a shared ring buffer read by an AudioWorklet.
 * Nothing here touches game logic.
 */
#include "gpu_backend.h"
#include "spu_backend.h"
#include "spu_core.h"
#include "ac3_input.h"
#include <emscripten.h>
#ifdef AC3_HW_WEBGPU
#include "hw/hw.h"
#include "hw/hw_glue.h"
#include "gpu_sw.h"
#include "pgxp.h"
#include "hw_packet.h"
#include "hw_fx_web.h"
#endif
#include "ac3_english.h"
#include "ac3_capture.h"
#include "coop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- JS glue */

EM_JS(int, web_quit_requested, (void), { return AC3W.quitRequested() ? 1 : 0; });
EM_JS(void, web_read_pad, (int *out), {
    /* out[0]=connected out[1]=type out[2]=buttons out[3..6]=lx,ly,rx,ry */
    const p = AC3W.padState();
    for (let i = 0; i < 7; i++) HEAP32[(out >> 2) + i] = p[i];
});
EM_JS(void, web_post_frame, (const u8 *rgba, int w, int h, int seq), {
    AC3W.postFrame(HEAPU8.subarray(rgba, rgba + w * h * 4), w, h, seq);
});
/* co-op: controller 2 (0 = none) and the guest's picture for the local split view */
EM_JS(int, web_read_pad2, (int *out), {
    const p = AC3W.pad2State ? AC3W.pad2State() : null;
    if (!p) return 0;
    for (let i = 0; i < 7; i++) HEAP32[(out >> 2) + i] = p[i];
    return 1;
});
/* own: 1 = the guest's own view, 0 = the host's picture (no guest view this field) */
EM_JS(void, web_post_frame2, (const u8 *rgba, int w, int h, int seq, int own), {
    if (AC3W.postFrame2) AC3W.postFrame2(HEAPU8.subarray(rgba, rgba + w * h * 4), w, h, seq, own);
});
EM_JS(void, web_sleep_ms, (double ms), { AC3W.sleep(ms); });
EM_JS(int, web_audio_queued, (void), { return AC3W.audioQueued(); });
EM_JS(int, web_audio_ok, (void), { return AC3W.audioOk() ? 1 : 0; });
EM_JS(void, web_audio_push, (const s16 *frames, int nframes), {
    AC3W.audioPush(HEAP16.subarray(frames >> 1, (frames >> 1) + nframes * 2));
});
EM_JS(void, web_card_changed, (int slot, const char *path), {
    AC3W.cardChanged(slot, UTF8ToString(path));
});
/* start-up progress for the page's loading screen: building the English data */
EM_JS(void, web_en_progress, (int disc, unsigned done, unsigned total), {
    if (AC3W.progress) AC3W.progress('english', disc, done, total);
});
static void en_progress(int disc, u32 done, u32 total) { web_en_progress(disc, done, total); }
__attribute__((constructor)) static void install_en_progress(void) { ac3_en_progress = en_progress; }
#ifdef AC3_HW_WEBGPU
/* Enhanced graphics: the GPU work runs in a separate render worker
 * (web/render-worker.js + render.wasm). Each field is sent there as one packet
 * (renderer/webgpu/hw_packet.h); see web/worker.js for the AC3W side. */
EM_JS(int, web_hd_wanted, (void), { return AC3W.hdWanted() ? 1 : 0; });
EM_JS(void, web_hd_send, (const u8 *p, int len, int seq), {
    AC3W.hdSend(HEAPU8.slice(p, p + len), seq);
});
EM_JS(void, web_hd_wait, (int seq), { AC3W.hdWait(seq); });
EM_JS(int, web_hd_first, (int seq), { return AC3W.hdFirst(seq) ? 1 : 0; });
EM_JS(int, web_hd_skipping, (int seq), { return AC3W.hdSkipping(seq) ? 1 : 0; });
EM_JS(int, web_hd_settings, (void), { return AC3W.hdSettings(); });
#endif

/* ------------------------------------------------------------------ input */

static void pad_from(ac3_pad_t *p, const int *s) {
    p->connected = 1;
    p->type = (u8)(s[1] ? AC3_PAD_ANALOG : AC3_PAD_DIGITAL);
    p->buttons = (u16)s[2];
    p->lx = (u8)s[3]; p->ly = (u8)s[4];
    p->rx = (u8)s[5]; p->ry = (u8)s[6];
}

static void web_input_poll(void) {
    int s[7];
    web_read_pad(s);
    pad_from(&ac3_pad[0], s);
    if (web_read_pad2(s)) pad_from(&ac3_pad[1], s);    /* co-op: the guest's controller */
}

/* ------------------------------------------------------------ frame pacing */

static double field_deadline_ms;

static void pace_field(void) {
    const double period = 1000.0 * (double)SPU_FIELD_NUM /
                          ((double)SPU_FIELD_DEN * (double)SPU_SAMPLE_RATE);
    double now = emscripten_get_now();
    if (field_deadline_ms == 0.0) { field_deadline_ms = now + period; return; }
    if (now < field_deadline_ms) web_sleep_ms(field_deadline_ms - now);
    field_deadline_ms += period;
    now = emscripten_get_now();
    if (field_deadline_ms + 4 * period < now) field_deadline_ms = now + period;
}

/* ------------------------------------------------------------ save hook */
/* runtime/hle/psyq_card.c calls this after each successful card write. */
void ac3_card_flushed(int slot, const char *path) { web_card_changed(slot, path); }

/* -------------------------------------------------------- display backend */

static u8 *rgba;
static int quit;
static u32 seq;

int ac3_render_quit_requested(void) { return quit; }
#ifdef AC3_ENHANCED
/* the native settings menu can relaunch the game (language change); the page
 * restarts the worker instead, so the game never asks for this here */
int ac3_render_restart_pending(int *english) { (void)english; return 0; }
int ac3_render_relaunch(int argc, char **argv, int english) { (void)argc; (void)argv; (void)english; return 0; }
#endif

#ifdef AC3_HW_WEBGPU
static int hd;                         /* 1 = enhanced renderer active */
static int frames_without_3d = 1000;

static void convert_display(const gpu_display_t *d);
static void convert_display_from(const gpu_display_t *d, const u16 *vram);
static void hd_skip_frame(const gpu_display_t *d0) {
    gpu_display_t d = *d0;
    if (d.w > 1024) d.w = 1024;
    if (d.h > 512) d.h = 512;
    if (d.enabled && d.w > 0 && d.h > 0 && rgba) { convert_display(&d); web_post_frame(rgba, d.w, d.h, (int)seq); }
    else web_post_frame(rgba, 0, 0, (int)seq);
}

static int hd_init(void) {
    if (!web_hd_wanted()) return 0;
    ac3_hd.forest = 0;    /* no forest in the web version (see hd_apply_settings) */
    hw_config_t cfg = { ac3_hd.scale, ac3_hd.perspective };
    hw_init_cpu(&cfg);
    hw_glue_attach();
    printf("display: enhanced (WebGPU in the render worker), %dx\n", ac3_hd.scale);
    return 1;
}

/* same decisions as present_field() in renderer/gpu_vulkan.c; the drawing
 * itself happens in the render worker */
/* graphics options from the page, changed live (web/app.js: GFX bits). The
 * renderer reads ac3_hd every field, as with the native settings menu. */
static void hd_apply_settings(void) {
    int b = web_hd_settings();
    if (b < 0) return;
    ac3_hd.widescreen = (b & 1) != 0;
    ac3_hd.fx         = (b & 2) != 0;
    ac3_hd.far_terrain = (b & 4) != 0;
    ac3_hd.ocean      = (b & 8) != 0;
    ac3_hd.clouds     = (b & 16) != 0;
    ac3_hd.forest     = 0;   /* removed from the web version: trees appeared on any ground (bit 32 unused) */
    ac3_hd.atmosphere = (b & 64) != 0;
    ac3_hd.gi         = (b & 128) != 0;
    ac3_hd.sun        = (b & 256) != 0;
    ac3_hd.game_fog   = (b & 512) != 0;
}

static void hd_frame(void) {
    hd_apply_settings();
    gpu_display_t d;
    gpu_display_state(&d);
    int use_hd = 0;
    if (gpu_sw_hooks) {
        if (hw_glue_widescreen_active() && ac3_pgxp_projections >= 32) frames_without_3d = 0;
        else if (frames_without_3d < 100000) frames_without_3d++;
        use_hd = !d.rgb24;
    }
    /* Movies: some (the Archnemesis briefing) are 15-bit and reach VRAM by a
     * path the enhanced renderer does not see, so its picture would freeze on
     * the last 3D frame. While the movie decoder is producing frames, show the
     * original picture, as for 24-bit movies. */
    {
        static u32 last_movie_frames;
        static int movie_fields = 1000;
        ac3_mdec_snap_t ms;
        ac3_mdec_snapshot(&ms);
        if (ms.frames != last_movie_frames) { last_movie_frames = ms.frames; movie_fields = 0; }
        else if (movie_fields < 1000) movie_fields++;
        if (movie_fields < 30) use_hd = 0;
    }
    int wide = use_hd && hw_glue_widescreen_active() && frames_without_3d <= 8;
    u32 flags = (use_hd ? PKT_HD : PKT_VRAM) | (wide ? PKT_WIDE : 0);
    if (web_hd_first((int)seq + 1)) {      /* first field drawn: give the GPU all of VRAM */
        PSX_RECT all = { 0, 0, 1024, 512 };
        hw_upload(&all, ac3_vram);
        hw_fx_resend_map();
    }
    u8 *p = NULL;
    size_t n = hw_packet_build(&p, ++seq, flags, &d, ac3_vram);
    hw_glue_frame_end(&d);
    if (web_hd_skipping((int)seq)) {        /* development fast-forward: show the original picture */
        hd_skip_frame(&d);
        return;
    }
    if (n) web_hd_send(p, (int)n, (int)seq);
    web_hd_wait((int)seq);    /* at most two fields ahead of the GPU */
}
#endif

/* co-op, guest's computer: its own picture (runtime/hle/coop_view.c); otherwise ac3_vram.
 * In the local split view (--coop-split) the main picture stays the host's and
 * the guest's goes out separately (web_frame_end). */
static void convert_display_from(const gpu_display_t *d, const u16 *vram);
static void convert_display(const gpu_display_t *d) {
    convert_display_from(d, ac3_coop_split ? ac3_vram : ac3_coop_display_vram(ac3_vram));
}

static void web_init(void) {
    rgba = (u8 *)malloc(1024 * 512 * 4);
#ifdef AC3_HW_WEBGPU
    hd = hd_init();
    if (hd) return;
#endif
    printf("display: web (software GPU, VRAM -> RGBA -> canvas)\n");
}
static void web_shutdown(void) {
#ifdef AC3_HW_WEBGPU
    if (hd) hw_shutdown();
#endif
    free(rgba); rgba = NULL;
}

/* Same conversion as renderer/shaders/psx.frag: 15-bit BGR555 or packed 24-bit. */
static void convert_display_from(const gpu_display_t *d, const u16 *vram) {
    int w = d->w, h = d->h;
    u8 *o = rgba;
    for (int y = 0; y < h; y++) {
        int vy = (d->y + y) & 511;
        const u16 *row = vram + vy * GPU_VRAM_W;
        if (d->rgb24) {
            for (int x = 0; x < w; x++) {
                int b = x * 3, wx = d->x + (b >> 1);
                u16 w0 = row[wx & 1023], w1 = row[(wx + 1) & 1023];
                if ((b & 1) == 0) { o[0] = w0 & 255; o[1] = w0 >> 8; o[2] = w1 & 255; }
                else              { o[0] = w0 >> 8;  o[1] = w1 & 255; o[2] = w1 >> 8; }
                o[3] = 255; o += 4;
            }
        } else {
            for (int x = 0; x < w; x++) {
                u16 c = row[(d->x + x) & 1023];
                u32 r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
                o[0] = (u8)((r << 3) | (r >> 2));
                o[1] = (u8)((g << 3) | (g >> 2));
                o[2] = (u8)((bl << 3) | (bl >> 2));
                o[3] = 255; o += 4;
            }
        }
    }
}

static void web_frame_begin(void) { }

static void web_frame_end(void) {
    if (web_quit_requested()) quit = 1;
    web_input_poll();

#ifdef AC3_HW_WEBGPU
    if (hd) { hd_frame(); pace_field(); return; }
#endif
    gpu_display_t d;
    gpu_display_state(&d);
    if (d.w > 1024) d.w = 1024;
    if (d.h > 512) d.h = 512;
    if (d.enabled && d.w > 0 && d.h > 0 && rgba) {
        convert_display(&d);
        web_post_frame(rgba, d.w, d.h, (int)++seq);
        if (ac3_coop_split) {                     /* local co-op test: the guest's view too */
            const u16 *g = ac3_coop_display_vram(ac3_vram);
            convert_display_from(&d, g);          /* the host's picture while the guest has none */
            web_post_frame2(rgba, d.w, d.h, (int)seq, g != ac3_vram);
        }
    } else {
        web_post_frame(rgba, 0, 0, (int)++seq);   /* display off: black */
        if (ac3_coop_split) web_post_frame2(rgba, 0, 0, (int)seq, 0);
    }
    pace_field();
}

static void web_set_draw_env(const PSX_DRAWENV *e) { (void)e; }
static void web_set_disp_env(const PSX_DISPENV *e) { (void)e; }
static void web_set_draw_area(s16 a, s16 b, s16 c, s16 d) { (void)a; (void)b; (void)c; (void)d; }
static void web_set_draw_offset(s16 x, s16 y) { (void)x; (void)y; }
static void web_set_draw_mode(u16 t, u16 m, u16 o) { (void)t; (void)m; (void)o; }
static void web_set_mask(int s, int c) { (void)s; (void)c; }
static void web_poly(const gpu_poly_t *p) { (void)p; }
static void web_rect(const gpu_rect_t *r) { (void)r; }
static void web_line(const gpu_line_t *l) { (void)l; }
static void web_load_image(const PSX_RECT *r, const void *d) { (void)r; (void)d; }
static void web_store_image(const PSX_RECT *r, void *d) { (void)r; (void)d; }
static void web_move_image(const PSX_RECT *r, s16 x, s16 y) { (void)r; (void)x; (void)y; }
static void web_fill_rect(const PSX_RECT *r, u8 a, u8 b, u8 c) { (void)r; (void)a; (void)b; (void)c; }

const gpu_backend_t gpu_backend_web = {
    "web",
    web_init, web_shutdown,
    web_frame_begin, web_frame_end,
    web_set_draw_env, web_set_disp_env,
    web_set_draw_area, web_set_draw_offset, web_set_draw_mode, web_set_mask,
    web_poly, web_rect, web_line,
    web_load_image, web_store_image, web_move_image, web_fill_rect,
};

/* Screenshots are a native debugging feature; not available in the browser. */
void ac3_render_screenshot(const char *path) { (void)path; }
void ac3_render_screenshot_when_drawing(const char *path, uint32_t min_prims) {
    (void)path; (void)min_prims;
}

/* ---------------------------------------------------------- audio backend */
/* Same queue policy as ac3_sdl_audio.c: keep ~4 fields queued, trim gently. */

#define TARGET_FRAMES   (SPU_FIELD_NOMINAL * 4)
#define CEILING_FRAMES  (SPU_FIELD_NOMINAL * 12)
#define TRIM_LIMIT      (SPU_FIELD_NOMINAL / 16)

static int primed;
static u32 underruns, overruns;

static void wa_init(void) { primed = 0; printf("audio: web AudioWorklet, %d Hz stereo\n", SPU_SAMPLE_RATE); }
static void wa_shutdown(void) { }

static int wa_frames_wanted(void) {
    if (!web_audio_ok()) return 0;
    int queued = web_audio_queued();
    if (!primed) { primed = 1; return TARGET_FRAMES; }
    if (queued == 0) underruns++;
    if (queued >= CEILING_FRAMES) { overruns++; return 0; }
    int trim = (TARGET_FRAMES - queued) / 16;
    if (trim > TRIM_LIMIT) trim = TRIM_LIMIT;
    if (trim < -TRIM_LIMIT) trim = -TRIM_LIMIT;
    int want = ac3_spu_field_frames() + trim;
    return want > 0 ? want : 0;
}

static void wa_submit(const s16 *frames, int nframes) {
    if (nframes > 0 && web_audio_ok()) web_audio_push(frames, nframes);
}

static void wa_key_on(int v, const spu_voice_t *voice) { (void)v; (void)voice; }
static void wa_key_off(int v) { (void)v; }
static void wa_upload(u32 a, const u8 *d, u32 s) { (void)a; (void)d; (void)s; }
static void wa_master(s16 l, s16 r) { (void)l; (void)r; }
static void wa_cdvol(s16 l, s16 r) { (void)l; (void)r; }

const spu_backend_t spu_backend_web = {
    "web",
    wa_init, wa_shutdown,
    wa_key_on, wa_key_off, wa_upload, wa_master, wa_cdvol,
    wa_frames_wanted, wa_submit,
};

void ac3_audio_queue_stats(u32 *under, u32 *over) {
    if (under) *under = underruns;
    if (over) *over = overruns;
}
