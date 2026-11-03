/*
 * hw_glue_internal.h -- shared by the glue files (not part of the public API
 * in hw_glue.h).
 *
 * The glue sits between the software GPU (gpu_sw.c) and the enhanced
 * renderer (hw.c): every primitive the game draws comes through the hooks in
 * hw_glue.c, which decide what the HD picture gets. It is split by topic:
 *
 *   hw_glue.c         the hooks, the draw-list phases, the world effects'
 *                     begin and end, and the public entry points
 *   hw_glue_fog.c     the game's fog bands and the fog colour and distances
 *   hw_glue_ops.c     primitives held back: the sky and flare phases
 *                     (buffered, then sorted out) and the see-through world
 *                     primitives (deferred until after the effects)
 *   hw_glue_diag.c    development aids: statistics, --glue-trace, --sun-test,
 *                     --cam-test
 *   hw_record.c       the F-key capture of a few fields (.ac3r files)
 *   hw_headless.c     native only: an off-screen Vulkan device
 *   hw_replay.c       native only: --replay, renders a capture to PNG files
 *   hw_selftest.c     native only: --selftest-hw
 *
 * Shared names are prefixed hw_glue_ in the object files; the short names
 * used in the code are the #defines below.
 */
#ifndef AC3_HW_GLUE_INTERNAL_H
#define AC3_HW_GLUE_INTERNAL_H
#include "hw_glue.h"
#include "hw_world.h"
#include "gpu_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GAME_GP 0x800BCB74u    /* the game's global pointer (its $gp) */

/* screen coordinates are 11-bit signed */
static inline s32 sx11(s32 v) { return (s32)((u32)v << 21) >> 21; }

/* ---- the draw list ---------------------------------------------------------
 * The game sorts its frame into an ordering table; which slot a primitive
 * came from tells what it is (g_node in hw_glue.c). */
enum { PH_NONE, PH_SKY, PH_TERRAIN, PH_OBJECTS, PH_FLARE, PH_HUD };

typedef struct {
    int flight;            /* this list draws the 3D world (a valid camera) */
    int fx;                /* world effects on for this list */
    int phase;             /* PH_* */
    int begun, ended;      /* world effects begun / ended this list */
    hw_scene_t scene;      /* the camera and lighting of this list */
    u32 ot[2];             /* the game's two ordering tables */
    u32 fields;            /* fields drawn so far */
    u32 order;             /* draw order of the world's primitives */
    int band_copies;       /* fog-band copies this list */
    int backdrop;          /* widescreen backdrop already drawn this frame */
    float fog[3];          /* the sky's haze colour */
} hw_glue_state_t;

extern hw_glue_state_t hw_glue_g;
#define G hw_glue_g

#define in_flight      hw_glue_in_flight
#define in_world       hw_glue_in_world
#define fx_flags_now   hw_glue_fx_flags_now
int in_flight(void);       /* the game is flying (not menus or the hangar) */
int in_world(void);        /* the terrain or objects phase */
u32 fx_flags_now(void);    /* FX_* for the current settings and map */

/* the drawing steps after the hooks (hw_replay.c and hw_selftest.c feed them
 * directly) */
#define glue_list_begin hw_glue_list_begin
#define glue_list_end   hw_glue_list_end
#define glue_phase      hw_glue_phase
#define glue_poly       hw_glue_poly
#define glue_rect       hw_glue_rect
#define glue_line       hw_glue_line
#define glue_fill       hw_glue_fill
#define glue_copy       hw_glue_copy
#define glue_upload     hw_glue_upload
#define resolve_pv      hw_glue_resolve_pv
#define glue_hooks      hw_glue_hooks
void glue_list_begin(const hw_scene_t *cam);
void glue_list_end(void);
void glue_phase(int ph);
void glue_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t *pv_in);
void glue_rect(const gpu_sw_state_t *st, const gpu_rect_t *r);
void glue_line(const gpu_sw_state_t *st, const gpu_line_t *l);
void glue_fill(const PSX_RECT *r, u16 pixel);
void glue_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy);
void glue_upload(const PSX_RECT *r);
void resolve_pv(const gpu_poly_t *p, hw_pv_t pv[4], const hw_pv_t *fallback);
extern const gpu_sw_hooks_t glue_hooks;

/* ---- fog bands (hw_glue_fog.c) -------------------------------------------- */
typedef struct { PSX_RECT src; s32 dx, dy; int n; } band_t;
#define MAX_BANDS 32

#define band_near       hw_glue_band_near
#define nband_near      hw_glue_nband_near
extern band_t band_near[MAX_BANDS];  /* last frame's series, each at its nearest table */
extern int nband_near;

#define fog_list_begin     hw_glue_fog_list_begin
#define fog_sample         hw_glue_fog_sample
#define fog_note_copy      hw_glue_fog_note_copy
#define fog_frame_end      hw_glue_fog_frame_end
#define bands_restart      hw_glue_bands_restart
#define bands_settle       hw_glue_bands_settle
#define band_candidate     hw_glue_band_candidate
#define band_note          hw_glue_band_note
#define band_nearest       hw_glue_band_nearest
#define band_largest       hw_glue_band_largest
#define bands_copy_nearest hw_glue_bands_copy_nearest
void fog_list_begin(hw_scene_t *s);
void fog_sample(const gpu_poly_t *p, const hw_pv_t *pv);
void fog_note_copy(const PSX_RECT *src, s32 dx, s32 dy);
void fog_frame_end(void);
void bands_restart(void);
void bands_settle(int keep);
int  band_candidate(const gpu_sw_state_t *st, const PSX_RECT *src);
void band_note(const PSX_RECT *src, s32 dx, s32 dy);
const PSX_RECT *band_nearest(const PSX_RECT *src, s32 dx, s32 dy);
const band_t *band_largest(void);
void bands_copy_nearest(void);

/* ---- primitives held back (hw_glue_ops.c) ----------------------------------
 * Each buffer_* and defer_* returns 0 when it could not keep the primitive
 * (out of memory): the caller then draws it at once. */
#define buffering        hw_glue_buffering
#define buffer_poly      hw_glue_buffer_poly
#define buffer_rect      hw_glue_buffer_rect
#define buffer_fill      hw_glue_buffer_fill
#define buffer_copy      hw_glue_buffer_copy
#define flush_ops        hw_glue_flush_ops
#define roofed           hw_glue_roofed
#define deferring        hw_glue_deferring
#define defer_poly       hw_glue_defer_poly
#define defer_rect       hw_glue_defer_rect
#define defer_line       hw_glue_defer_line
#define defer_copy       hw_glue_defer_copy
#define flush_deferred   hw_glue_flush_deferred
#define ops_reset        hw_glue_ops_reset
#define discard_deferred hw_glue_discard_deferred
int  buffering(void);
int  buffer_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]);
int  buffer_rect(const gpu_sw_state_t *st, const gpu_rect_t *r);
int  buffer_fill(const PSX_RECT *r, u16 pixel);
int  buffer_copy(const gpu_sw_state_t *st, const PSX_RECT *r, s32 dx, s32 dy);
void flush_ops(int phase);
int  roofed(void);
int  deferring(void);
int  defer_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]);
int  defer_rect(const gpu_sw_state_t *st, const gpu_rect_t *r);
int  defer_line(const gpu_sw_state_t *st, const gpu_line_t *l);
int  defer_copy(const gpu_sw_state_t *st, const PSX_RECT *r, s32 dx, s32 dy);
void flush_deferred(void);
void ops_reset(void);
void discard_deferred(void);

/* ---- statistics and development aids (hw_glue_diag.c) --------------------- */
typedef struct { int sprites, rings, strip, hidden, roof, lists, sun, sea, deferred; } hw_glue_stats_t;
extern hw_glue_stats_t hw_glue_stats;
#define gstat hw_glue_stats

#define glue_verbose  hw_glue_verbose
#define diag_world    hw_glue_diag_world
#define diag_depth    hw_glue_diag_depth
#define diag_rect     hw_glue_diag_rect
#define diag_line     hw_glue_diag_line
#define diag_report   hw_glue_diag_report
#define sun_test      hw_glue_sun_test
#define cam_test      hw_glue_cam_test
#define tracing       hw_glue_tracing
#define trace_poly    hw_glue_trace_poly
#define trace_rect    hw_glue_trace_rect
#define trace_fill    hw_glue_trace_fill
#define trace_copy    hw_glue_trace_copy
#define trace_upload  hw_glue_trace_upload
extern int glue_verbose;   /* print statistics per list (the replay tool) */
void diag_world(const gpu_poly_t *p, const hw_pv_t *pv);
void diag_depth(const gpu_poly_t *p, const hw_pv_t *pv);
void diag_rect(const gpu_rect_t *r);
void diag_line(void);
void diag_report(void);
extern float hw_sun_test[2];
extern int hw_sun_test_on;
extern float hw_cam_test[5];
extern int hw_cam_test_on;
extern int hw_cam_sweep_n;
extern float hw_cam_sweep_d[5];
void sun_test(hw_scene_t *s);
void cam_test(hw_scene_t *s);     /* also places the sun with --sun-test */
extern u32 hw_glue_trace_field;
int  tracing(void);
void trace_poly(const gpu_poly_t *p);
void trace_rect(const gpu_rect_t *r);
void trace_fill(const PSX_RECT *r, u16 pixel);
void trace_copy(const PSX_RECT *src, s32 dx, s32 dy);
void trace_upload(const PSX_RECT *r);

/* ---- capture recording (hw_record.c) ---------------------------------------
 * A capture is a header, the whole VRAM, then records of a type byte, a
 * 4-byte length and the data. */
#define REC_MAGIC   0x52334341u
#define REC_VERSION 2u
#define REC_FIELDS  4          /* fields per capture */
enum { R_POLY = 1, R_RECT, R_LINE, R_FILL, R_COPY, R_UPLOAD, R_FIELD, R_GTE,
       R_LIST, R_PHASE, R_RAM };

#define rec_active hw_glue_rec_active
#define rec_poly   hw_glue_rec_poly
#define rec_rect   hw_glue_rec_rect
#define rec_line   hw_glue_rec_line
#define rec_fill   hw_glue_rec_fill
#define rec_copy   hw_glue_rec_copy
#define rec_upload hw_glue_rec_upload
#define rec_list   hw_glue_rec_list
#define rec_phase  hw_glue_rec_phase
#define rec_field  hw_glue_rec_field
int  rec_active(void);
void rec_poly(const gpu_poly_t *p, const hw_pv_t pv[4]);
void rec_rect(const gpu_rect_t *r);
void rec_line(const gpu_line_t *l);
void rec_fill(const PSX_RECT *r, u16 pixel);
void rec_copy(const PSX_RECT *src, s32 dx, s32 dy);
void rec_upload(const PSX_RECT *r);
void rec_list(int begin, const hw_scene_t *cam);
void rec_phase(int ph);
void rec_field(const gpu_display_t *d);

/* ---- native tools: an off-screen Vulkan device (hw_headless.c) ------------ */
#ifndef AC3_HW_WEBGPU
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qf;
    VkQueue q;
    VkCommandPool pool;
    VkCommandBuffer cb;
} hw_headless_t;
extern hw_headless_t hw_headless;
#define HV hw_headless
extern double hw_headless_flush_ms;    /* GPU time of the last flush */

#define headless_init     hw_headless_init
#define headless_flush    hw_headless_flush
#define headless_shutdown hw_headless_shutdown
#define write_rgba_png    hw_write_rgba_png
#define write_sw_png      hw_write_sw_png
#define write_depth_png   hw_write_depth_png

int  headless_init(int scale);
void headless_flush(void);
void headless_shutdown(void);
void write_rgba_png(const char *path, const u8 *rgba, unsigned w, unsigned h);
void write_sw_png(const char *path, int x, int y, int w, int h);
void write_depth_png(const char *path, const float *z, unsigned w, unsigned h);
#endif

#endif
