/*
 * hw_fx_game.c -- the CPU half of the world effects, in the game worker.
 *
 * The logic is renderer/hw/hw_fx.c's, unchanged: scenes are queued by
 * hw_glue.c (hw_fx_begin/end), and at the end of each field the uniform block
 * of every scene, the far-terrain tile lists and (when the map changed) the
 * map data are packed for the render worker (hw_fx_web.h), which owns the GPU
 * side (hw_fx_webgpu.c). Keep in step with hw_fx.c: queue(), hw_fx_palette(),
 * cloud_history(), fill_ubo(), gather_tiles(), ocean_horizon(), the scene
 * loop of hw_fx_prepare(). The ocean's CPU parts are in hw_ocean_web.c.
 */
#include "hw_fx_web.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS hw_state

u32 hw_fx_flags = FX_FAR | FX_OCEAN | FX_CLOUDS | FX_FOREST | FX_ATMO | FX_GI | FX_SUN;

#define CLOUD_HISTORY 0.9f
#define Z_FAR         2.0e6f

static struct {
    int ready;
    hw_scene_t scenes[FX_MAX_SCENES];
    u8 kind[FX_MAX_SCENES];
    s32 pal[FX_MAX_SCENES][6], pend_pal[6];
    u32 nscenes;
    struct {
        int valid;
        u32 frame;
        float rot[9], cam[3], H, ws, ofs[2];
        s32 area[4];
    } cl_last;
    u8 ocean_on[FX_MAX_SCENES];
    float oparams[HW_OCEAN_PARAMS][4];
    double ocean_last;
    hw_ocean_node_t nodes[2][HW_OCEAN_MAX_NODES];
    /* this field's results, packed by hw_fx_pack() */
    fx_ubo_t ubo[FX_MAX_SCENES];
    tile_draw_t draws[2][FX_MAX_DRAWS];
    float inst[2][FX_MAX_INST * 2];
    fx_pkt_t hdr;
    u32 sent_version;
    int map_sent;
} F;

int hw_fx_init(void) {
    memset(&F, 0, sizeof F);
    hw_ocean_spectrum(NULL, F.oparams);
    F.ready = 1;
    return 1;
}

void hw_fx_shutdown(void) { memset(&F, 0, sizeof F); }

static void queue(u8 op, const hw_scene_t *s) {
    if (!F.ready || F.nscenes >= FX_MAX_SCENES) return;
    F.scenes[F.nscenes] = *s;
    F.kind[F.nscenes] = op;
    memcpy(F.pal[F.nscenes], F.pend_pal, sizeof F.pend_pal);
    cmd_t *c = hw_new_cmd(op);
    if (!c) return;
    c->first = F.nscenes++;
}

void hw_fx_palette(s32 dx, s32 dy, s32 w, s32 h, s32 sx, s32 sy) {
    s32 p[6] = { dx, dy, w, h, sx, sy };
    memcpy(F.pend_pal, p, sizeof p);
}

void hw_fx_begin(const hw_scene_t *s) { queue(OP_FX_BEGIN, s); }

void hw_fx_end(const hw_scene_t *s) {
    queue(OP_FX_END, s);
    hw_mark_dirty(s->area[0], s->area[1], s->area[2] + 1, s->area[3] + 1);
}

int  hw_fx_ready(void) { return F.ready; }
void hw_fx_frame_done(void) { F.nscenes = 0; }
/* the ocean always has compute in WebGPU (native also checks the Vulkan queue) */
int  hw_fx_ocean_drawn(void) {
    return F.ready && (hw_fx_flags & FX_OCEAN) && (hw_fx_flags & FX_FAR) && hw_map.valid && hw_map.has_sea;
}

static float ocean_horizon(const hw_scene_t *s) {
    double alt = (double)s->sea_y - (double)s->cam[1];
    if (alt < 1.0) alt = 1.0;
    double h = sqrt(2.0 * 6.371e6 * 5.0 * alt) * 1.15 + 20000.0;
    if (h > 2.2e6) h = 2.2e6;
    return (float)h;
}
void hw_fx_resend_map(void) { F.map_sent = 0; }

static void cloud_history(fx_ubo_t *u, const hw_scene_t *s) {
    int ok = F.cl_last.valid && s->frame > F.cl_last.frame && s->frame - F.cl_last.frame <= 8 &&
             fabsf(s->H - F.cl_last.H) <= 0.01f * s->H && fabsf(s->ws - F.cl_last.ws) <= 0.01f;
    float moved = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = s->cam[i] - F.cl_last.cam[i];
        moved += d * d;
    }
    if (moved > 5000.0f * 5000.0f) ok = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) u->prev_wtv[4 * i + k] = F.cl_last.rot[3 * k + i];
    for (int i = 0; i < 3; i++) u->prev_cam[i] = F.cl_last.cam[i];
    u->prev_proj[0] = F.cl_last.H;
    u->prev_proj[1] = F.cl_last.ws;
    u->prev_proj[2] = F.cl_last.ofs[0];
    u->prev_proj[3] = F.cl_last.ofs[1];
    for (int i = 0; i < 4; i++) u->prev_area[i] = (float)F.cl_last.area[i];
    u->cloud_tc[0] = (float)(s->frame & 1023u);
    u->cloud_tc[1] = ok ? CLOUD_HISTORY : 0.0f;
    F.cl_last.valid = 1;
    F.cl_last.frame = s->frame;
    memcpy(F.cl_last.rot, s->rot, sizeof F.cl_last.rot);
    memcpy(F.cl_last.cam, s->cam, sizeof F.cl_last.cam);
    F.cl_last.H = s->H;
    F.cl_last.ws = s->ws;
    F.cl_last.ofs[0] = s->ofs[0];
    F.cl_last.ofs[1] = s->ofs[1];
    memcpy(F.cl_last.area, s->area, sizeof F.cl_last.area);
}

static void fill_ubo(fx_ubo_t *u, const hw_scene_t *s, u32 si, float tile_half) {
    memset(u, 0, sizeof *u);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            u->vtw[4 * i + k] = s->rot[3 * i + k];
            u->wtv[4 * i + k] = s->rot[3 * k + i];
        }
    u->cam[0] = s->cam[0]; u->cam[1] = s->cam[1]; u->cam[2] = s->cam[2]; u->cam[3] = s->time;
    u->proj[0] = s->H; u->proj[1] = s->ws; u->proj[2] = (float)HS.S; u->proj[3] = s->sea_y;
    u->ofs[0] = s->ofs[0]; u->ofs[1] = s->ofs[1];
    u->ofs[2] = (float)s->area[0]; u->ofs[3] = (float)s->area[1];
    u->area[0] = (float)(s->area[2] - s->area[0] + 1);
    u->area[1] = (float)(s->area[3] - s->area[1] + 1);
    u->area[2] = s->fog_z[0];             /* original fog: fade start, end (0: off) */
    u->area[3] = s->fog_z[1];
    float horizon[3];
    hw_world_sky_at(s, 0.1f, horizon);
    float lum = horizon[0] * 0.3f + horizon[1] * 0.59f + horizon[2] * 0.11f;
    float inten = lum * 2.4f;
    if (inten < 0.15f) inten = 0.15f;
    if (inten > 1.6f) inten = 1.6f;
    float up = -s->sun[1];
    if (up < 0.05f) inten *= up <= -0.05f ? 0.0f : (up + 0.05f) / 0.1f;
    for (int i = 0; i < 3; i++) {
        u->sun[i] = s->sun[i];
        u->sun_col[i] = s->sun_col[i];
        u->fog[i] = s->fog_col[i];
        u->water[i] = (float)((hw_map.water_rgb >> (8 * i)) & 255u) / 255.0f;
    }
    u->sun[3] = inten;
    u->fog[3] = 1.0f / 110000.0f;
    u->map[0] = hw_map.valid ? 1.0f : 0.0f;
    u->map[1] = hw_map.top_y;
    u->map[2] = 300.0f;
    u->map[3] = 1.4e6f;
    u->cloud[0] = s->sea_y - 7500.0f;
    u->cloud[1] = s->sea_y - 13500.0f;
    u->cloud[2] = 0.36f;
    u->cloud[3] = 1.0f;
    u->wind[0] = s->time * 30.0f; u->wind[1] = s->time * 12.0f;
    u->wind[2] = 0.8253f; u->wind[3] = 0.5646f;
    u->water[3] = hw_map.valid && hw_map.has_sea ? 1.0f : 0.0f;
    u->misc[0] = 1.0f / (float)HS.W; u->misc[1] = 1.0f / (float)HS.Hs;
    u->misc[2] = Z_FAR;
    u32 flags = hw_fx_flags;
    if (!hw_map.valid) flags &= ~(FX_FAR | FX_FOREST);
    u->misc[3] = (float)flags;
    for (int i = 0; i < HW_SKY_BINS; i++)
        for (int c = 0; c < 3; c++) u->sky[i][c] = s->sky[i][c];
    for (int c = 0; c < 3; c++) u->sky[c][3] = s->game_fog_col[c];   /* original fog colour */
    for (int i = 0; i < 4; i++) u->pal[i] = (float)F.pal[si][i];
    u->pal_src[0] = (float)F.pal[si][4];
    u->pal_src[1] = (float)F.pal[si][5];
    u->pal_src[3] = tile_half;
    u->hdr[0] = (float)(512u * (u32)HS.S); u->hdr[1] = (float)(256u * (u32)HS.S);
    u->hdr[2] = 0.5f;
    u->hdr[3] = 1.0f;
    memcpy(u->ocean, F.oparams, sizeof u->ocean);
    u->ocean[1][3] = F.ocean_on[si] ? 1.0f : 0.0f;
}

static u32 gather_tiles(const hw_scene_t *s, float *inst, tile_draw_t *draws, u32 *ninst) {
    static u16 cell_type[FX_MAX_INST];
    static float cell_org[FX_MAX_INST][2];
    static u32 count[512], start[512];
    memset(count, 0, sizeof count);
    int ctx = (int)floorf(s->cam[0] / HW_TILE_SIZE) + 128;
    int ctz = (int)floorf(s->cam[2] / HW_TILE_SIZE) + 128;
    float hw_ = (float)(s->area[2] - s->area[0] + 1) * 0.5f * 1.15f / (s->H * s->ws);
    float hh = (float)(s->area[3] - s->area[1] + 1) * 0.5f * 1.15f / s->H;
    float nx = 1.0f / sqrtf(1.0f + hw_ * hw_), ny = 1.0f / sqrtf(1.0f + hh * hh);
    const float R = 2600.0f;
    u32 n = 0;
    for (int dz = -FX_TILE_CELLS; dz <= FX_TILE_CELLS; dz++)
        for (int dx = -FX_TILE_CELLS; dx <= FX_TILE_CELLS; dx++) {
            int cx = ctx + dx, cz = ctz + dz;
            int wx = cx < 0 ? (cx & 7) : cx >= HW_MAP_N ? ((cx & 7) | 0xF8) : cx;
            int wz = cz < 0 ? (cz & 7) : cz >= HW_MAP_N ? ((cz & 7) | 0xF8) : cz;
            u16 t = hw_map.grid[wz * HW_MAP_N + wx];
            if (!hw_map.mesh_count[t] || hw_map.tile_water[t]) continue;
            float ox = (float)(cx - 128) * HW_TILE_SIZE + HW_TILE_SIZE * 0.5f;
            float oz = (float)(cz - 128) * HW_TILE_SIZE + HW_TILE_SIZE * 0.5f;
            float rel[3] = { ox - s->cam[0], hw_map.tile_top[t] * 0.5f + hw_map.sea_y * 0.5f - s->cam[1],
                             oz - s->cam[2] };
            float v[3];
            for (int i = 0; i < 3; i++)
                v[i] = s->rot[3 * i] * rel[0] + s->rot[3 * i + 1] * rel[1] + s->rot[3 * i + 2] * rel[2];
            if (v[2] < -R) continue;
            if ((v[0] - hw_ * v[2]) * nx > R || (-v[0] - hw_ * v[2]) * nx > R) continue;
            if ((v[1] - hh * v[2]) * ny > R || (-v[1] - hh * v[2]) * ny > R) continue;
            cell_type[n] = t;
            cell_org[n][0] = ox;
            cell_org[n][1] = oz;
            count[t]++;
            n++;
        }
    u32 nd = 0, acc = 0;
    for (int t = 0; t < 512; t++) {
        start[t] = acc;
        if (!count[t]) continue;
        draws[nd].first_vtx = hw_map.mesh_first[t];
        draws[nd].nvtx = hw_map.mesh_count[t];
        draws[nd].first_inst = acc;      /* relative to this scene's instances */
        draws[nd].ninst = count[t];
        nd++;
        acc += count[t];
    }
    for (u32 i = 0; i < n; i++) {
        u32 k = start[cell_type[i]]++;
        inst[2 * k] = cell_org[i][0];
        inst[2 * k + 1] = cell_org[i][1];
    }
    *ninst = n;
    return nd;
}

/* hw_fx_prepare()'s CPU part, for this field */
size_t hw_fx_pack_size(void) {
    fx_pkt_t *h = &F.hdr;
    memset(h, 0, sizeof *h);
    if (!F.ready) return 0;
    u32 mesh_n = hw_map.valid && hw_map.mesh && hw_map.mesh_n < FX_MESH_CAP ? hw_map.mesh_n : 0;
    /* the ocean: simulation time and the water patches of up to two scenes (hw_fx_prepare) */
    int sim = 0;
    double sim_time = 0.0;
    for (u32 i = 0; i < F.nscenes && !sim; i++)
        if (F.kind[i] == OP_FX_BEGIN && (hw_fx_flags & FX_OCEAN) && hw_map.valid && hw_map.has_sea) {
            sim = 1;
            sim_time = F.scenes[i].time;
        }
    if (sim) {
        double dt = sim_time - F.ocean_last;
        if (dt < 0.0 || dt > 1.0) dt = 1.0 / 30.0;
        F.ocean_last = sim_time;
        h->ocean_sim = 1;
        h->ocean_t = (float)fmod(sim_time, HW_OCEAN_REPEAT_S);
        h->ocean_dt = (float)dt;
    }
    int no = 0;
    h->ocean_scene[0] = h->ocean_scene[1] = -1;
    memset(F.ocean_on, 0, sizeof F.ocean_on);
    for (u32 i = 0; i < F.nscenes; i++)
        if (F.kind[i] == OP_FX_BEGIN && no < 2 && hw_fx_ocean_drawn()) {
            h->nnodes[no] = hw_ocean_select(&F.scenes[i], ocean_horizon(&F.scenes[i]), F.nodes[no],
                                            HW_OCEAN_MAX_NODES);
            h->ocean_scene[no] = (s32)i;
            F.ocean_on[i] = 1;
            no++;
        }
    for (u32 i = 0, last = 0; i < F.nscenes; i++) {
        if (F.kind[i] == OP_FX_BEGIN) last = F.ocean_on[i];
        else F.ocean_on[i] = (u8)last;
    }
    int nb = 0;
    h->tiles_scene[0] = h->tiles_scene[1] = -1;
    for (u32 i = 0; i < F.nscenes; i++) {
        float half = 0.0f;
        if (F.kind[i] == OP_FX_BEGIN && nb < 2 && (hw_fx_flags & FX_FAR) && hw_map.valid &&
            mesh_n && F.pal[i][2] > 0) {
            h->ndraws[nb] = gather_tiles(&F.scenes[i], F.inst[nb], F.draws[nb], &h->ninst[nb]);
            h->tiles_scene[nb] = (s32)i;
            half = (float)FX_TILE_CELLS * HW_TILE_SIZE - HW_TILE_SIZE;
            nb++;
        }
        fx_ubo_t *u = &F.ubo[i];
        fill_ubo(u, &F.scenes[i], i, half);
        if (F.kind[i] == OP_FX_END) {
            if (hw_fx_flags & FX_CLOUDS) cloud_history(u, &F.scenes[i]);
            else F.cl_last.valid = 0;
        }
        h->kind[i] = F.kind[i];
        memcpy(h->area[i], F.scenes[i].area, sizeof h->area[i]);
    }
    h->nscenes = F.nscenes;
    h->flags = hw_fx_flags;
    h->map_valid = hw_map.valid ? 1u : 0u;
    h->forest = hw_map.valid && hw_map.forest_samples > 0;
    if (hw_map.valid && (!F.map_sent || F.sent_version != hw_map.version)) {
        h->mesh_n = mesh_n;
        h->map_bytes = (u32)FX_MAP_BYTES(mesh_n);
    }
    return sizeof *h + (size_t)F.nscenes * sizeof(fx_ubo_t) +
           (size_t)(h->ndraws[0] + h->ndraws[1]) * sizeof(tile_draw_t) +
           (size_t)(h->ninst[0] + h->ninst[1]) * 8 +
           (size_t)(h->nnodes[0] + h->nnodes[1]) * sizeof(hw_ocean_node_t) + h->map_bytes;
}

void hw_fx_pack(u8 *p) {
    fx_pkt_t *h = &F.hdr;
    memcpy(p, h, sizeof *h);                                   p += sizeof *h;
    memcpy(p, F.ubo, (size_t)h->nscenes * sizeof(fx_ubo_t));   p += (size_t)h->nscenes * sizeof(fx_ubo_t);
    for (int b = 0; b < 2; b++) {
        memcpy(p, F.draws[b], (size_t)h->ndraws[b] * sizeof(tile_draw_t));
        p += (size_t)h->ndraws[b] * sizeof(tile_draw_t);
    }
    for (int b = 0; b < 2; b++) {
        memcpy(p, F.inst[b], (size_t)h->ninst[b] * 8);
        p += (size_t)h->ninst[b] * 8;
    }
    for (int b = 0; b < 2; b++) {
        memcpy(p, F.nodes[b], (size_t)h->nnodes[b] * sizeof(hw_ocean_node_t));
        p += (size_t)h->nnodes[b] * sizeof(hw_ocean_node_t);
    }
    if (h->map_bytes) {
        memcpy(p, hw_map.grid, sizeof hw_map.grid);           p += sizeof hw_map.grid;
        memcpy(p, hw_map.tile_top, sizeof hw_map.tile_top);   p += sizeof hw_map.tile_top;
        memcpy(p, hw_map.height, sizeof hw_map.height);       p += sizeof hw_map.height;
        memcpy(p, hw_map.color, sizeof hw_map.color);         p += sizeof hw_map.color;
        if (h->mesh_n) memcpy(p, hw_map.mesh, (size_t)h->mesh_n * sizeof(hw_tile_vtx_t));
        F.map_sent = 1;
        F.sent_version = hw_map.version;
    }
}
