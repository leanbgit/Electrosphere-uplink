/*
 * hw_replay.c -- native tools only: --replay renders a capture (hw_record.c)
 * with the enhanced renderer, field by field, into PNG files.
 */
#ifndef AC3_HW_WEBGPU
#include "hw_glue_internal.h"

extern void ac3_vram_init(void);

/* one record of the capture */
typedef struct { u8 t; u32 len; const u8 *p; } rec_ref_t;

/* Captures from before the list and phase records (version 1): where each
 * list begins and ends, and where its terrain and HUD start, are guessed from
 * the primitives (the precise vertices, the first fog band, the first line). */
enum { EV_BEGIN = 1, EV_TERRAIN = 2, EV_HUD = 4, EV_END = 8 };

static void guess_phases(const rec_ref_t *r, int n, u8 *ev) {
    size_t st = sizeof(gpu_sw_state_t), pl = sizeof(gpu_poly_t);
    for (int i = 0; i < n; ) {
        if (r[i].t == R_FIELD || r[i].t == R_GTE) { i++; continue; }
        int j = i, polys = 0, first_prec = -1, last_prec = -1, first_band = -1, first_line = -1;
        for (; j < n && r[j].t != R_FIELD; j++) {
            if (r[j].t == R_POLY) {
                polys++;
                hw_pv_t pv[4];
                memcpy(pv, r[j].p + st + pl, sizeof pv);
                if (pv[0].ok || pv[1].ok || pv[2].ok) {
                    if (first_prec < 0) first_prec = j;
                    last_prec = j;
                }
            } else if (r[j].t == R_LINE && first_band >= 0 && first_line < 0) {
                first_line = j;
            } else if (r[j].t == R_COPY && first_band < 0) {
                PSX_RECT s;
                memcpy(&s, r[j].p + st, sizeof s);
                if (s.w == 16 && (s.h == 64 || s.h == 16)) first_band = j;
            }
        }
        if (polys >= 64 && first_prec >= 0) {
            int terr = first_band >= 0 ? first_band : first_prec;
            ev[i] |= EV_BEGIN;
            ev[terr] |= EV_TERRAIN;
            int hud = last_prec + 1;
            if (first_line > terr && first_line < hud) hud = first_line;
            if (hud < j) ev[hud] |= EV_HUD;
            ev[j < n ? j : n - 1] |= EV_END;
        }
        i = j;
    }
}

/* The fog series are known from the frame before; a replay has none, so they
 * are taken from the whole capture first. */
static void prime_bands(const rec_ref_t *r, int n, const u8 *ev) {
    int ph = PH_NONE, copies = 0;
    size_t st = sizeof(gpu_sw_state_t);
    bands_restart();
    for (int i = 0; i < n; i++) {
        if (ev[i] & EV_TERRAIN) ph = PH_TERRAIN;
        if (ev[i] & EV_HUD) ph = PH_HUD;
        if (r[i].t == R_PHASE) { u32 p; memcpy(&p, r[i].p, 4); ph = (int)p; }
        if (r[i].t == R_LIST) { u32 b; memcpy(&b, r[i].p, 4); if (!b) ph = PH_NONE; }
        if (r[i].t == R_COPY && (ph == PH_TERRAIN || ph == PH_OBJECTS)) {
            PSX_RECT s; s32 dx, dy;
            gpu_sw_state_t gs;
            memcpy(&gs, r[i].p, st);
            memcpy(&s, r[i].p + st, sizeof s);
            memcpy(&dx, r[i].p + st + sizeof s, 4);
            memcpy(&dy, r[i].p + st + sizeof s + 4, 4);
            if (!band_candidate(&gs, &s)) continue;
            band_note(&s, dx, dy);
            copies++;
        }
    }
    bands_settle(copies >= 4);
    bands_restart();
}

/* --replay CAPTURE: draws each field of a capture again and writes it as
 * PNG files (HD picture, its depth, the game's own picture), with the glue's
 * statistics. ram_path: the game RAM to use if the capture has none. */
int hw_replay_main(const char *path, const char *out_prefix, const char *ram_path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 1; }
    fclose(f);
    const u32 *hdr = (const u32 *)data;
    if (size < 16 || hdr[0] != REC_MAGIC || hdr[1] < 1 || hdr[1] > REC_VERSION) {
        fprintf(stderr, "replay: %s is not a recording\n", path);
        return 1;
    }
    size_t vbytes = (size_t)GPU_VRAM_W * GPU_VRAM_H * 2;
    if ((size_t)size < 16 + vbytes) return 1;

    ac3_vram_init();
    memcpy(ac3_vram, data + 16, vbytes);
    gpu_sw_reset();
    if (!headless_init(ac3_hd.scale)) {
        fprintf(stderr, "replay: no Vulkan device\n");
        return 1;
    }
    int S = hw_scale();
    PSX_RECT all = { 0, 0, GPU_VRAM_W, GPU_VRAM_H };
    hw_upload(&all, ac3_vram);
    hw_fx_flags = fx_flags_now();
    printf("replay: %s, %d fields recorded, widescreen %s, scale %d\n",
           path, (int)hdr[2], hdr[3] ? "on" : "off", S);

    int have_ram = 0;
    if (ram_path) {
        FILE *rf = fopen(ram_path, "rb");
        if (rf) {
            have_ram = fread(ac3_ram, 1, AC3_RAM_SIZE, rf) == AC3_RAM_SIZE;
            fclose(rf);
        }
        printf("replay: game RAM from %s%s\n", ram_path, have_ram ? "" : " (unreadable)");
    }

    int nrec = 0, caprec = 4096;
    rec_ref_t *recs = malloc(sizeof *recs * (size_t)caprec);
    int has_lists = 0;
    for (size_t q = 16 + vbytes; q + 5 <= (size_t)size; ) {
        u8 t = data[q];
        u32 l;
        memcpy(&l, data + q + 1, 4);
        if (q + 5 + l > (size_t)size) break;
        if (nrec == caprec) { caprec *= 2; recs = realloc(recs, sizeof *recs * (size_t)caprec); }
        recs[nrec].t = t;
        recs[nrec].len = l;
        recs[nrec].p = data + q + 5;
        nrec++;
        if (t == R_LIST) has_lists = 1;
        if (t == R_RAM && l == AC3_RAM_SIZE) { memcpy(ac3_ram, data + q + 5, AC3_RAM_SIZE); have_ram = 1; }
        q += 5 + l;
    }
    u8 *ev = calloc((size_t)nrec + 1, 1);
    if (!has_lists && have_ram) guess_phases(recs, nrec, ev);
    prime_bands(recs, nrec, ev);

    static u32 gte_keys[1 << 17];
    static u8 gte_used[1 << 17];
    memset(gte_used, 0, sizeof gte_used);
    u32 n_gte = 0, v_total = 0, v_from_gte = 0;
    for (int i = 0; i < nrec; i++) {
        if (recs[i].t != R_GTE || recs[i].len < 4) continue;
        u32 k;
        memcpy(&k, recs[i].p, 4);
        u32 h = (k * 2654435761u) >> 15;
        while (gte_used[h] && gte_keys[h] != k) h = (h + 1) & ((1u << 17) - 1);
        if (!gte_used[h]) { gte_used[h] = 1; gte_keys[h] = k; n_gte++; }
    }

    int field = 0, map_png = 0, pass = 0;
    glue_verbose = 1;
    hw_world_verbose = 1;
next_pass:
    for (int i = 0; i < nrec; i++) {
        u8 type = recs[i].t;
        const u8 *p = recs[i].p;
        if (ev[i] & EV_BEGIN) {
            hw_scene_t cam;
            memset(&cam, 0, sizeof cam);
            ac3_ws_factor = hdr[3] ? 0.75f : 1.0f;
            if (ac3_hd.fx) hw_world_map_update();
            hw_world_camera(&cam);
            glue_list_begin(&cam);
            glue_phase(PH_SKY);
        }
        if (ev[i] & (EV_TERRAIN | EV_HUD)) {
            if (type == R_POLY || type == R_RECT || type == R_LINE || type == R_COPY)
                memcpy(&gpu_sw, p, sizeof gpu_sw);
            if (ev[i] & EV_TERRAIN) glue_phase(PH_TERRAIN);
            if (ev[i] & EV_HUD) glue_phase(PH_HUD);
        }
        if ((ev[i] & EV_END) && type == R_FIELD) glue_list_end();
        switch (type) {
        case R_POLY: {
            gpu_sw_state_t st; gpu_poly_t pr; hw_pv_t pv[4];
            memcpy(&st, p, sizeof st); memcpy(&pr, p + sizeof st, sizeof pr);
            memcpy(pv, p + sizeof st + sizeof pr, sizeof pv);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS"))) {
                static s32 la[4] = { -1, -1, -1, -1 };
                if (la[0] != st.area_x0 || la[1] != st.area_y0 || la[2] != st.area_x1 || la[3] != st.area_y1) {
                    printf("TMP area %d,%d-%d,%d ofs %d,%d (op %d)\n", st.area_x0, st.area_y0, st.area_x1, st.area_y1, st.ofs_x, st.ofs_y, i);
                    la[0] = st.area_x0; la[1] = st.area_y0; la[2] = st.area_x1; la[3] = st.area_y1;
                }
                int ymin = 9999, ymax = -9999;
                for (int k = 0; k < pr.nverts; k++) { int y = pr.y[k] + st.ofs_y; if (y < ymin) ymin = y; if (y > ymax) ymax = y; }
                int by = st.area_y0;
                if (G.phase == PH_FLARE)
                    printf("TMP flarepoly op %d n%d tex%d semi%d gour%d y %d..%d rgb %02x%02x%02x tpage %04x\n", i, pr.nverts, pr.textured, pr.semitrans, pr.gouraud, ymin, ymax, pr.r[0], pr.g[0], pr.b[0], pr.tpage);
                else if (ymin < by + 32 || ymax >= by + 192)
                    printf("TMP poly op %d n%d tex%d semi%d y %d..%d rgb %02x%02x%02x pv %d\n", i, pr.nverts, pr.textured, pr.semitrans, ymin - by, ymax - by, pr.r[0], pr.g[0], pr.b[0], pv[0].ok);
            }
            gpu_sw = st; gpu_sw_poly(&pr);
            if (n_gte && ac3_hd.pgxp) {
                hw_pv_t rec_pv[4];
                memcpy(rec_pv, pv, sizeof pv);
                resolve_pv(&pr, pv, rec_pv);
            }
            if (!ac3_hd.pgxp) memset(pv, 0, sizeof pv);
            glue_poly(&st, &pr, pv);
            if (n_gte)
                for (int k = 0; k < (pr.nverts == 4 ? 4 : 3); k++) {
                    u32 key = ((u32)(u16)pr.y[k] << 16) | (u16)pr.x[k];
                    u32 h = (key * 2654435761u) >> 15;
                    while (gte_used[h] && gte_keys[h] != key) h = (h + 1) & ((1u << 17) - 1);
                    v_total++;
                    if (gte_used[h]) v_from_gte++;
                }
            break;
        }
        case R_RECT: {
            gpu_sw_state_t st; gpu_rect_t r;
            memcpy(&st, p, sizeof st); memcpy(&r, p + sizeof st, sizeof r);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP rect op %d at %d,%d %ux%u tex%d semi%d rgb %02x%02x%02x area %d,%d-%d,%d\n", i, r.x + st.ofs_x, r.y + st.ofs_y, r.w, r.h, r.textured, r.semitrans, r.r, r.g, r.b, st.area_x0, st.area_y0, st.area_x1, st.area_y1);
            gpu_sw = st; gpu_sw_rect(&r);
            glue_rect(&st, &r);
            break;
        }
        case R_LINE: {
            gpu_sw_state_t st; static gpu_line_t l;
            memcpy(&st, p, sizeof st); memcpy(&l, p + sizeof st, sizeof l);
            gpu_sw = st; gpu_sw_line(&l);
            glue_line(&st, &l);
            break;
        }
        case R_FILL: {
            PSX_RECT r; u16 px;
            memcpy(&r, p, sizeof r); memcpy(&px, p + sizeof r, 2);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP fill op %d at %d,%d %dx%d px %04x\n", i, r.x, r.y, r.w, r.h, px);
            gpu_sw_fill(&r, px);
            glue_fill(&r, px);
            break;
        }
        case R_COPY: {
            gpu_sw_state_t st; PSX_RECT r; s32 dx, dy;
            memcpy(&st, p, sizeof st); memcpy(&r, p + sizeof st, sizeof r);
            memcpy(&dx, p + sizeof st + sizeof r, 4); memcpy(&dy, p + sizeof st + sizeof r + 4, 4);
            gpu_sw = st; gpu_sw_copy(&r, dx, dy);
            glue_copy(&st, &r, dx, dy);
            break;
        }
        case R_UPLOAD: {
            PSX_RECT r;
            memcpy(&r, p, sizeof r);
            const u8 *px = p + sizeof r;
            for (s32 y = 0; y < r.h; y++)
                for (s32 x = 0; x < r.w; x++)
                    memcpy(vram_at((r.x + x) & 1023, (r.y + y) & 511),
                           px + ((size_t)y * r.w + x) * 2, 2);
            glue_upload(&r);
            break;
        }
        case R_LIST: {
            u32 begin;
            hw_scene_t scene;
            memcpy(&begin, p, 4);
            memcpy(&scene, p + 4, sizeof scene);
            if (begin) {
                if (ac3_hd.fx && have_ram) hw_world_map_update();
                glue_list_begin(&scene);
            } else {
                glue_list_end();
            }
            break;
        }
        case R_PHASE: {
            u32 ph;
            memcpy(&ph, p, 4);
            memcpy(&gpu_sw, p + 4, sizeof gpu_sw);
            if (getenv("AC3_TMP_OPS") && field == atoi(getenv("AC3_TMP_OPS")))
                printf("TMP phase %u at op %d (area %d,%d-%d,%d) begun %d ended %d\n", ph, i, gpu_sw.area_x0, gpu_sw.area_y0, gpu_sw.area_x1, gpu_sw.area_y1, G.begun, G.ended);
            glue_phase((int)ph);
            break;
        }
        case R_GTE: {
            u32 key;
            ac3_pgxp_vtx_t v;
            if (recs[i].len < 4 + sizeof v) break;
            memcpy(&key, p, 4);
            memcpy(&v, p + 4, sizeof v);
            ac3_pgxp_record(key, &v);
            break;
        }
        case R_FIELD: {
            gpu_display_t d;
            memcpy(&d, p, sizeof d);
            ac3_pgxp_next_frame();
            headless_flush();
            G.fields++;
            u32 prims = 0, precise = 0;
            hw_frame_stats(&prims, &precise);
            char out[300];
            if (hw_map.valid && !map_png) {
                snprintf(out, sizeof out, "%s_map.png", out_prefix);
                hw_world_map_png(out);
                map_png = 1;
            }
            if (d.enabled && d.w > 0 && d.h > 0 && !d.rgb24) {
                unsigned W = (unsigned)(d.w * S), Hh = (unsigned)(d.h * S);
                u8 *rgba = malloc((size_t)W * Hh * 4);
                float *z = malloc((size_t)W * Hh * 4);
                if (rgba && z &&
                    hw_readback(HV.q, HV.pool, d.x, d.y, d.w, d.h, rgba) &&
                    hw_readback_depth(HV.q, HV.pool, d.x, d.y, d.w, d.h, z)) {
                    snprintf(out, sizeof out, "%s_f%d.png", out_prefix, field);
                    write_rgba_png(out, rgba, W, Hh);
                    if (hw_cam_sweep_n <= 1) {
                        snprintf(out, sizeof out, "%s_f%d_depth.png", out_prefix, field);
                        write_depth_png(out, z, W, Hh);
                    }
                }
                free(rgba); free(z);
                if (hw_cam_sweep_n <= 1) {
                    snprintf(out, sizeof out, "%s_f%d_native.png", out_prefix, field);
                    write_sw_png(out, d.x, d.y, d.w, d.h);
                }
                if (getenv("AC3_CLUTDUMP")) {
                    static u8 cb[64 * 4 * 16 * 4];
                    int SS = hw_scale();
                    if (hw_readback(HV.q, HV.pool, 352, 299, 16, 1, cb)) {
                        printf("HD clut row 299:");
                        for (int i = 0; i < 16; i++) { u8 *q = cb + (size_t)(i * SS) * 4; printf(" %02X%02X%02X/%d", q[0], q[1], q[2], q[3] > 127); }
                        printf("\nSW clut row 299:");
                        for (int i = 0; i < 16; i++) { u16 p = *vram_at(352 + i, 299); printf(" %02X%02X%02X/%d", (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3, p >> 15); }
                        printf("\n");
                    }
                    if (hw_readback(HV.q, HV.pool, 448, 256, 4, 16, cb)) {
                        printf("HD idx page:");
                        for (int j = 2; j < 8; j++) for (int i = 0; i < 4; i++) { u8 *q = cb + ((size_t)(j * SS) * (4 * SS) + (size_t)(i * SS)) * 4; printf(" %02X%02X", q[0], q[1]); }
                        printf("\nSW idx page:");
                        for (int j = 2; j < 8; j++) for (int i = 0; i < 4; i++) { u16 p = *vram_at(448 + i, 256 + j); printf(" %04X", p); }
                        printf("\n");
                    }
                }
            }
            printf("  field %d: display %d,%d %dx%d%s, %u primitives, %u with precise vertices,"
                   " %.1f ms on the GPU\n",
                   field, d.x, d.y, d.w, d.h, d.rgb24 ? " 24-bit" : "", prims, precise, hw_headless_flush_ms);
            field++;
            break;
        }
        default:
            break;
        }
    }
    if (++pass < hw_cam_sweep_n) {
        for (int k = 0; k < 5; k++) hw_cam_test[k] += hw_cam_sweep_d[k];
        memcpy(ac3_vram, data + 16, vbytes);
        hw_upload(&all, ac3_vram);
        glue_verbose = hw_world_verbose = 0;
        goto next_pass;
    }
    if (n_gte)
        printf("  the GTE made %u distinct projections; %u of %u polygon vertices are one of them\n",
               n_gte, v_from_gte, v_total);
    else
        printf("  (recorded before projections were captured)\n");
    headless_shutdown();
    free(ev);
    free(recs);
    free(data);
    return 0;
}

#endif /* !AC3_HW_WEBGPU */
