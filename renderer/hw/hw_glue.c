/*
 * hw_glue.c -- between the software GPU and the enhanced renderer.
 *
 * The software GPU (gpu_sw.c) still draws every frame into the PlayStation's
 * VRAM; through gpu_sw_hooks it also hands each primitive here, and this file
 * decides what the HD picture (hw.c) gets:
 *
 *  - the draw list's phases: the game sorts its frame into an ordering table,
 *    and the slot a primitive came from says what it is -- sky, terrain,
 *    objects, the lens flare, the HUD (g_node);
 *  - the world effects (far terrain, ocean, clouds, haze, sun: hw_world.c and
 *    the effect renderers) begin when the terrain starts and end before the
 *    flare and HUD (begin_world, end_world);
 *  - precise vertices: each corner's unrounded position and depth from the
 *    GTE (resolve_pv), and a depth from the map for terrain corners the GTE
 *    did not give (terrain_depth);
 *  - what is held back or left out: the sky and flare phases are buffered,
 *    see-through world primitives deferred (hw_glue_ops.c), fog-band copies
 *    replaced (hw_glue_fog.c), the game's sea left out under the ocean effect.
 *
 * The other glue files are listed in hw_glue_internal.h.
 */
#include "hw_glue_internal.h"

hw_glue_state_t G = { .fog = { 0.55f, 0.6f, 0.6f } };

int in_flight(void) {
    u32 g = MEM_R32(GAME_GP + 748);
    if ((g & 0x1FFFFFu) == 0 || (g >> 24) != 0x80u) return 0;
    return (int)MEM_R32(g + 44) == 2;
}

int in_world(void) {
    return G.phase == PH_TERRAIN || G.phase == PH_OBJECTS;
}

u32 fx_flags_now(void) {
    u32 f = 0;
    if (ac3_hd.far_terrain && ac3_hd.atmosphere) f |= FX_FAR;
    if (ac3_hd.ocean)      f |= FX_OCEAN;
    if (ac3_hd.clouds)     f |= FX_CLOUDS;
    if (ac3_hd.forest)     f |= FX_FOREST;
    if (ac3_hd.atmosphere) f |= FX_ATMO;
    if (ac3_hd.gi)         f |= FX_GI;
    if (ac3_hd.sun)        f |= FX_SUN;
    if (roofed())          f &= ~(FX_CLOUDS | FX_SUN);
    return f;
}

/* ---- the world effects ---------------------------------------------------- */

static void begin_world(void) {
    G.begun = 1;
    if (!G.fx) return;
    if (hw_cam_test_on) cam_test(&G.scene);
    G.scene.ofs[0] = (float)gpu_sw.ofs_x;
    G.scene.ofs[1] = (float)gpu_sw.ofs_y;
    G.scene.area[0] = gpu_sw.area_x0; G.scene.area[1] = gpu_sw.area_y0;
    G.scene.area[2] = gpu_sw.area_x1; G.scene.area[3] = gpu_sw.area_y1;
    G.scene.time = (float)G.fields / 60.0f;
    G.scene.frame = G.fields;
    if (hw_map.valid && hw_map.has_sea) G.scene.sea_y = hw_map.sea_y;
    hw_fx_flags = fx_flags_now();
    /* the haze takes its colours from the largest fog series' nearest table */
    const band_t *pb = band_largest();
    if (pb && ac3_hd.atmosphere)
        hw_fx_palette(pb->dx, pb->dy, pb->src.w, pb->src.h, pb->src.x, pb->src.y);
    else
        hw_fx_palette(0, 0, 0, 0, 0, 0);
    hw_fx_begin(&G.scene);
    if (ac3_hd.atmosphere && !ac3_hd.game_fog && nband_near) bands_copy_nearest();
}

static void end_world(void) {
    G.ended = 1;
    if (!G.fx || !G.begun) { discard_deferred(); return; }
    hw_fx_end(&G.scene);
    flush_deferred();
}

/* Widescreen shows more of the world to the left and right than the game
 * thinks is on screen, so it leaves out the terrain tiles there. With world
 * effects on, the far-terrain pass paints the ground underneath, but not
 * every part of the gaps; anything not drawn would show pixels left over from
 * earlier frames as smeared streaks.
 * So the HD picture's drawing area is first cleared to the haze colour. Only
 * the HD picture: the game's own VRAM is not touched. */
static void widescreen_backdrop(void) {
    G.backdrop = 1;
    if (!hw_glue_widescreen_active()) return;
    /* Also with the far terrain on: it does not cover everything. Underground
     * (the Geofront missions) there is no sky, and the sides above the terrain
     * kept old pixels (HUD fragments). The effects draw over the fill wherever
     * they do reach, so the fill is invisible elsewhere. */
    PSX_RECT r = { (s16)gpu_sw.area_x0, (s16)gpu_sw.area_y0,
                   (s16)(gpu_sw.area_x1 - gpu_sw.area_x0 + 1), (s16)(gpu_sw.area_y1 - gpu_sw.area_y0 + 1) };
    if (r.w <= 0 || r.h <= 0) return;
    u16 c[3];
    for (int i = 0; i < 3; i++) {
        float v = G.fog[i] < 0.0f ? 0.0f : G.fog[i] > 1.0f ? 1.0f : G.fog[i];
        c[i] = (u16)lrintf(v * 31.0f);
    }
    hw_fill(&r, (u16)(c[0] | (c[1] << 5) | (c[2] << 10)));
}

/* ---- the draw list and its phases ----------------------------------------- */

void glue_phase(int ph) {
    if (!G.flight || ph == G.phase) return;
    if (!G.backdrop && ph >= PH_SKY && ph <= PH_OBJECTS) widescreen_backdrop();
    int old = G.phase;
    if (old == PH_SKY || old == PH_FLARE) flush_ops(old);
    if (old == PH_SKY) {
        memcpy(G.scene.fog_col, G.fog, sizeof G.fog);
        hw_world_sky_finish(&G.scene);
    }
    G.phase = ph;
    if (!G.begun && (ph == PH_TERRAIN || ph == PH_OBJECTS)) begin_world();
    if (!G.ended && (ph == PH_FLARE || ph == PH_HUD)) end_world();
}

/* cam: the game's camera for this list (not valid outside flight) */
void glue_list_begin(const hw_scene_t *cam) {
    G.flight = cam && cam->valid;
    G.phase = PH_NONE;
    G.begun = G.ended = 0;
    G.backdrop = 0;
    G.band_copies = 0;
    G.order = 0;
    hw_draw_order(0);
    ops_reset();
    bands_restart();
    if (!G.flight) { G.fx = 0; return; }
    G.scene = *cam;
    if (hw_sun_test_on) sun_test(&G.scene);
    memcpy(G.scene.fog_col, G.fog, sizeof G.fog);
    fog_list_begin(&G.scene);
    hw_world_sky_begin(&G.scene);
    G.fx = ac3_hd.fx && hw_map.valid;
}

void glue_list_end(void) {
    if (G.flight) {
        if (G.phase == PH_SKY || G.phase == PH_FLARE) flush_ops(G.phase);
        if (G.phase == PH_SKY) hw_world_sky_finish(&G.scene);
        if (G.begun && !G.ended) end_world();
        if (G.band_copies >= 4) {          /* a frame with fog: its series are known now */
            fog_frame_end();
            bands_settle(1);
        }
        diag_report();
    }
    G.flight = 0;
    G.fx = 0;
    G.phase = PH_NONE;
}

/* ---- precise vertices ----------------------------------------------------- */

/* Terrain corners without a precise vertex get their depth from the map (a
 * ray through the pixel), if it fits the polygon's other corners. */
static void terrain_depth(const gpu_poly_t *p, hw_pv_t *pv) {
    int n = p->nverts == 4 ? 4 : 3;
    float zmin = 1e30f, zmax = 0.0f;
    for (int i = 0; i < n; i++)
        if (pv[i].ok) {
            if (pv[i].v.vz < zmin) zmin = pv[i].v.vz;
            if (pv[i].v.vz > zmax) zmax = pv[i].v.vz;
        }
    if (zmax <= 0.0f) return;
    for (int i = 0; i < n; i++) {
        if (pv[i].ok) continue;
        float sx = (float)sx11(p->x[i]), sy = (float)sx11(p->y[i]);
        float z = hw_world_raycast(&G.scene, sx, sy, 32.0f, 60000.0f);
        if (z <= 0.0f || z < zmin * 0.5f || z > zmax * 2.0f) continue;
        pv[i].ok = 1;
        pv[i].v.sx = sx;
        pv[i].v.sy = sy;
        pv[i].v.vz = z;
        pv[i].v.vx = sx * z / (G.scene.H * G.scene.ws);
        pv[i].v.vy = sy * z / G.scene.H;
    }
}

/* Each corner's precise vertex: the GTE projections that landed on that
 * screen position this frame. When several did, the one closest to the
 * corners already settled is taken (fewest candidates first). fallback: the
 * vertices recorded with the polygon (replays). */
#define PV_CAND 32
void resolve_pv(const gpu_poly_t *p, hw_pv_t pv[4], const hw_pv_t *fallback) {
    static ac3_pgxp_vtx_t c[4][PV_CAND];
    int n = p->nverts == 4 ? 4 : 3, nc[4], fixed[4];
    memset(pv, 0, sizeof(hw_pv_t) * 4);
    for (int i = 0; i < n; i++) {
        u32 key = ((u32)(u16)p->y[i] << 16) | (u16)p->x[i];
        u32 gen[PV_CAND];
        nc[i] = ac3_pgxp_lookup_all(key, c[i], gen, PV_CAND);
        if (!nc[i] && fallback && fallback[i].ok) { c[i][0] = fallback[i].v; nc[i] = 1; }
        fixed[i] = nc[i] == 1;
        if (fixed[i]) { pv[i].v = c[i][0]; pv[i].ok = 1; }
    }
    for (int round = 0; round < n; round++) {
        int pick = -1;
        for (int i = 0; i < n; i++)
            if (!fixed[i] && nc[i] > 1 && (pick < 0 || nc[i] < nc[pick])) pick = i;
        if (pick < 0) break;
        float cx = 0, cy = 0, cz = 0;
        int nf = 0;
        for (int i = 0; i < n; i++)
            if (fixed[i] && pv[i].ok) { cx += pv[i].v.vx; cy += pv[i].v.vy; cz += pv[i].v.vz; nf++; }
        float cost[PV_CAND];
        float best_d = 1e30f;
        for (int k = 0; k < nc[pick]; k++) {
            const ac3_pgxp_vtx_t *q = &c[pick][k];
            float d = 0;
            if (nf) {
                float dx = q->vx - cx / nf, dy = q->vy - cy / nf, dz = q->vz - cz / nf;
                d = dx * dx + dy * dy + dz * dz;
            } else {
                for (int j = 0; j < n; j++) {
                    if (j == pick || !nc[j]) continue;
                    float m = 1e30f;
                    for (int l = 0; l < nc[j]; l++) {
                        float dx = q->vx - c[j][l].vx, dy = q->vy - c[j][l].vy, dz = q->vz - c[j][l].vz;
                        float e = dx * dx + dy * dy + dz * dz;
                        if (e < m) m = e;
                    }
                    d += m;
                }
            }
            cost[k] = d;
            if (d < best_d) best_d = d;
        }
        int best = 0;
        for (int k = 0; k < nc[pick]; k++) {
            float tol = 0.05f * c[pick][k].vz;
            if (cost[k] <= best_d * 4.0f + tol * tol) { best = k; break; }
        }
        pv[pick].v = c[pick][best];
        pv[pick].ok = 1;
        fixed[pick] = 1;
    }
}

/* ---- each primitive ------------------------------------------------------- */

/* the game's sea polygon, at sea level, where the ocean effect draws instead */
static int game_sea(const gpu_poly_t *p, const hw_pv_t *pv) {
    if (!p->textured || !hw_fx_ocean_drawn() || !hw_world_sea_texture(p->tpage, p->clut)) return 0;
    const float *R = G.scene.rot;
    int n = p->nverts == 4 ? 4 : 3, ok = 0;
    for (int i = 0; i < n; i++) {
        if (!pv[i].ok) continue;
        float y = R[1] * pv[i].v.vx + R[4] * pv[i].v.vy + R[7] * pv[i].v.vz + G.scene.cam[1];
        if (fabsf(y - hw_map.sea_y) > 12.0f) return 0;
        ok++;
    }
    return ok > 0;
}

/* the next place in the world's draw order (0 outside the world) */
static u32 next_order(void) {
    return G.flight && in_world() ? ++G.order : 0;
}

void glue_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t *pv_in) {
    hw_draw_order(next_order());
    hw_pv_t pv[4];
    if (pv_in) memcpy(pv, pv_in, sizeof pv);
    else memset(pv, 0, sizeof pv);
    if (G.flight) {
        if (G.phase == PH_SKY && !p->textured)        /* the sky's gradient colours */
            for (int i = 0; i < (p->nverts == 4 ? 4 : 3); i++)
                hw_world_sky_add(&G.scene, (float)sx11(p->x[i]), (float)sx11(p->y[i]),
                                 p->r[i], p->g[i], p->b[i]);
        if (in_world() && G.fx && hw_map.valid) {
            terrain_depth(p, pv);
            fog_sample(p, pv);
            diag_world(p, pv);
            if (G.begun && game_sea(p, pv)) { gstat.sea++; return; }
        }
        if (hw_cam_test_on && in_world()) return;
        if (p->semitrans && deferring() && defer_poly(st, p, pv)) return;
        diag_depth(p, pv);
    }
    if (buffering() && buffer_poly(st, p, pv)) return;
    hw_poly(st, p, pv);
}

void glue_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    hw_draw_order(next_order());
    diag_rect(r);
    if (r->semitrans && deferring() && defer_rect(st, r)) return;
    if (G.flight && G.phase == PH_SKY && !r->textured && r->w >= 16 && r->h >= 16) {
        G.fog[0] = r->r / 255.0f;                     /* the sky's backdrop: the haze colour */
        G.fog[1] = r->g / 255.0f;
        G.fog[2] = r->b / 255.0f;
    }
    if (buffering() && buffer_rect(st, r)) return;
    hw_rect(st, r);
}

void glue_line(const gpu_sw_state_t *st, const gpu_line_t *l) {
    hw_draw_order(next_order());
    diag_line();
    if (l->semitrans && deferring() && defer_line(st, l)) return;
    if (buffering()) flush_ops(G.phase);
    hw_line(st, l);
}

void glue_fill(const PSX_RECT *r, u16 pixel) {
    if (buffering() && buffer_fill(r, pixel)) return;
    hw_fill(r, pixel);
}

void glue_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy) {
    PSX_RECT s = *src;
    if (G.flight && in_world() && band_candidate(st, src)) {     /* a fog band */
        band_note(src, dx, dy);
        G.band_copies++;
        fog_note_copy(src, dx, dy);
        if (G.fx && ac3_hd.atmosphere && !ac3_hd.game_fog) {
            const PSX_RECT *n = band_nearest(src, dx, dy);
            if (n) s = *n;
        }
    }
    if (buffering() && buffer_copy(st, &s, dx, dy)) return;
    /* a copy from the picture itself (the heat haze) waits with the see-through
     * primitives it belongs to */
    if (!getenv("AC3_NODEFERCOPY") && deferring() && s.x < st->area_x1 + 1 && s.x + s.w > st->area_x0 &&
        s.y < st->area_y1 + 1 && s.y + s.h > st->area_y0 && defer_copy(st, &s, dx, dy))
        return;
    hw_copy(st, &s, dx, dy);
}

void glue_upload(const PSX_RECT *r) {
    if (buffering()) flush_ops(G.phase);
    hw_upload(r, ac3_vram);
}

/* ---- the hooks (gpu_sw.c calls them for everything it draws) -------------- */

static void g_poly(const gpu_poly_t *p) {
    trace_poly(p);
    hw_pv_t pv[4];
    memset(pv, 0, sizeof pv);
    if (ac3_hd.pgxp) resolve_pv(p, pv, NULL);
    rec_poly(p, pv);
    glue_poly(&gpu_sw, p, pv);
}

static void g_rect(const gpu_rect_t *r) {
    trace_rect(r);
    rec_rect(r);
    glue_rect(&gpu_sw, r);
}

static void g_line(const gpu_line_t *l) {
    rec_line(l);
    glue_line(&gpu_sw, l);
}

static void g_fill(const PSX_RECT *r, u16 pixel) {
    trace_fill(r, pixel);
    rec_fill(r, pixel);
    glue_fill(r, pixel);
}

static void g_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    trace_copy(src, dx, dy);
    rec_copy(src, dx, dy);
    glue_copy(&gpu_sw, src, dx, dy);
}

static void g_upload(const PSX_RECT *r) {
    trace_upload(r);
    rec_upload(r);
    glue_upload(r);
}

/* A draw list begins or ends. In flight, the list's ordering tables and the
 * camera come from the game's world structure. */
static void g_list(int begin) {
    hw_scene_t cam;
    memset(&cam, 0, sizeof cam);
    if (begin) {
        G.ot[0] = G.ot[1] = 0;
        if (in_flight()) {
            u32 world = MEM_R32(GAME_GP + 736);
            if ((world >> 24) == 0x80u) {
                G.ot[0] = MEM_R32(world + 24);
                G.ot[1] = MEM_R32(world + 28);
            }
            if (ac3_hd.fx) hw_world_map_update();
            hw_world_camera(&cam);
        }
    }
    rec_list(begin, &cam);
    if (begin) glue_list_begin(&cam);
    else glue_list_end();
}

/* An ordering-table slot is reached: slot 4095 is the sky, 4094 the terrain,
 * 2 and up the objects, 1 the lens flare, 0 the HUD. */
static void g_node(u32 addr, int len) {
    if (!G.flight || len != 0) return;
    int ph = -1;
    for (int k = 0; k < 2; k++) {
        u32 base = G.ot[k];
        if (!base || addr < base || addr >= base + 4u * 4096u || ((addr - base) & 3u)) continue;
        u32 slot = (addr - base) >> 2;
        ph = slot == 4095 ? PH_SKY : slot == 4094 ? PH_TERRAIN : slot >= 2 ? PH_OBJECTS
           : slot == 1 ? PH_FLARE : PH_HUD;
    }
    if (ph < 0 || ph == G.phase) return;
    if (tracing()) printf("trace f%u phase %d -> %d\n", G.fields, G.phase, ph);
    rec_phase(ph);
    glue_phase(ph);
}

const gpu_sw_hooks_t glue_hooks = { g_poly, g_rect, g_line, g_fill, g_copy, g_upload,
                                    g_list, g_node };

/* ---- public --------------------------------------------------------------- */

void hw_glue_attach(void) {
    gpu_sw_hooks = &glue_hooks;
    ac3_pgxp_enabled = ac3_hd.pgxp;
    ac3_ws_factor = 1.0f;
    hw_fx_flags = fx_flags_now();
}

int hw_glue_widescreen_active(void) {
    return ac3_ws_factor != 1.0f;
}

void hw_set_pgxp(int on) {
    ac3_hd.pgxp = on ? 1 : 0;
    ac3_pgxp_enabled = ac3_hd.pgxp;
}

void hw_glue_toggle_fx(void) {
    ac3_hd.fx = !ac3_hd.fx;
    printf("enhanced: world effects %s\n", ac3_hd.fx ? "on" : "off");
}

void hw_glue_frame_end(const gpu_display_t *d) {
    ac3_pgxp_next_frame();
    ac3_ws_factor = (ac3_hd.widescreen && in_flight()) ? 0.75f : 1.0f;
    ac3_gte_no_depthcue = ac3_hd.fx && ac3_hd.atmosphere && hw_map.valid && in_flight();
    if (hw_glue_trace_field && G.fields + 5 >= hw_glue_trace_field && G.fields < hw_glue_trace_field + 3)
        printf("trace: end of glue field %u (flight %d)\n", G.fields, G.flight);
    G.fields++;
    rec_field(d);
}
