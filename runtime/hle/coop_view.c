/* coop_view.c -- the guest's own picture (docs/coop/S3_view.md).
 *
 * Right after the player update (host and guest done), RAM and registers are
 * saved. The rest of the frame (world, HUD preparation, camera, drawing) runs
 * normally for the host; after its last drawing call the rest of the frame is
 * replayed from the saved state with the guest swapped in as "the player",
 * and then the host's memory is put back. The simulation therefore stays
 * identical to the host's on both computers; only the picture differs. On
 * the guest's computer the guest's picture is kept and shown instead. */
#include "coop_internal.h"
#include "gpu_backend.h"

#define PICTURE_MAX    0x40000u   /* largest picture buffer (ordering table + packets) */
#define OT_END         0xFFFFFFu  /* GPU packet link: end of the list */

/* the picture buffer the frame is drawing into: ordering table + packets */
static int draw_buffer(u32 *lo, u32 *hi) {
    u32 w = MEM_R32(WORLD_PTR);
    if (!ram_ptr(w)) return 0;
    u32 ot = MEM_R32(w + WORLD_OT), ot0 = MEM_R32(w + WORLD_OT0), ot1 = MEM_R32(w + WORLD_OT1);
    u32 pk0 = MEM_R32(w + WORLD_PACKETS0), pk1 = MEM_R32(w + WORLD_PACKETS1);
    if (!ram_ptr(ot0) || !ram_ptr(ot1) || !ram_ptr(pk0) || !ram_ptr(pk1) || ot1 <= pk0) return 0;
    u32 pk_size = ot1 - pk0;                 /* the two buffers sit back to back */
    if (ot == ot0) { *lo = ot0; *hi = pk0 + pk_size; }
    else if (ot == ot1) { *lo = ot1; *hi = pk1 + pk_size; }
    else return 0;
    return *hi > *lo && ((*hi - 0x80000000u) <= AC3_RAM_SIZE) && *hi - *lo <= PICTURE_MAX;
}

/* Drawing packets the guest's picture uses outside its buffer (the HUD keeps
 * its own): found by walking the ordering table like the GPU will, copied
 * out before the host's memory is put back and copied in afterwards. */
#define MAX_EXTRA_BYTES 0x20000u
static struct { u32 addr, len; } extra[4096];
static unsigned n_extra;
static u8 extra_bytes[MAX_EXTRA_BYTES];

static void collect_packets(u32 lo, u32 hi, u32 ot_base, u32 ot_entries) {
    n_extra = 0;
    u32 used = 0, steps = 0;
    u32 a = ot_base + (ot_entries - 1u) * 4u;          /* ClearOTagR: drawn from the last entry */
    while (steps++ < 400000u) {
        if ((a >> 24) != 0x80u || (a & AC3_RAM_MASK) + 4u > AC3_RAM_SIZE) break;
        u32 word = MEM_R32(a), len = (word >> 24) * 4u + 4u;
        if ((a < lo || a >= hi) && n_extra < sizeof extra / sizeof extra[0] && used + len <= MAX_EXTRA_BYTES) {
            extra[n_extra].addr = a;
            extra[n_extra].len = len;
            memcpy(extra_bytes + used, ac3_ram + (a & AC3_RAM_MASK), len);
            used += len;
            n_extra++;
        }
        if ((word & OT_END) == OT_END) break;          /* end of the list */
        a = 0x80000000u | (word & OT_END);           /* the next packet */
    }
}

/* ---- showing the guest's picture -------------------------------------------
 * The guest's picture never enters the game's memory: it is laid over it
 * only while the GPU reads the frame (DrawOTag at the start of the next
 * frame), then the game's own bytes are put back. So both computers' game
 * memory stays byte-identical; only what reaches the screen differs. */
static struct { int ready, active; u32 lo, hi; } OV;
static u8 extra_backup[MAX_EXTRA_BYTES];
static u8 *ov_backup;

static void apply_overlay(void) {
    u8 *ram = ac3_ram;
    memcpy(ov_backup, ram + (OV.lo & AC3_RAM_MASK), OV.hi - OV.lo);
    memcpy(ram + (OV.lo & AC3_RAM_MASK), G.keep_buf, OV.hi - OV.lo);
    u32 used = 0;
    for (unsigned i = 0; i < n_extra; i++) {
        u8 *at = ram + (extra[i].addr & AC3_RAM_MASK);
        memcpy(extra_backup + used, at, extra[i].len);
        memcpy(at, extra_bytes + used, extra[i].len);
        used += extra[i].len;
    }
}

int ac3_coop_gpu_ready(void) { return ac3_coop_view && OV.ready && ov_backup; }

int ac3_coop_gpu_draw(int begin) {
    if (begin) {
        if (!ac3_coop_view || !OV.ready || !ov_backup) return 0;
        apply_overlay();
        OV.active = 1;
        return 1;
    } else if (OV.active) {
        /* in reverse order, so overlapping packets come back right */
        u32 used = 0;
        for (unsigned i = 0; i < n_extra; i++) used += extra[i].len;
        for (unsigned i = n_extra; i-- > 0; ) {
            used -= extra[i].len;
            memcpy(ac3_ram + (extra[i].addr & AC3_RAM_MASK), extra_backup + used, extra[i].len);
        }
        memcpy(ac3_ram + (OV.lo & AC3_RAM_MASK), ov_backup, OV.hi - OV.lo);
        OV.active = 0;
        OV.ready = 0;
    }
    return 0;
}

/* The guest's computer keeps two video memories. The game's own (ac3_vram)
 * receives exactly the host's picture, so whatever the game reads back from
 * the screen is the same on both computers. The display copy receives the
 * guest's picture and is what gets shown. */
static u16 *disp_vram;
static u32 disp_field;
#define VRAM_BYTES (1024u * 512u * 2u)

u16 *ac3_coop_display_pass(const u16 *game_vram) {
    int fresh = disp_vram != NULL && ac3_vblank_count() - disp_field <= 4u;
    if (!disp_vram) disp_vram = (u16 *)malloc(VRAM_BYTES);
    if (!disp_vram) return NULL;
    /* Bring textures and everything else up to date from the game's VRAM,
     * except the picture on screen right now: the game double-buffers, so
     * that is the guest's previous picture (this pass draws the other one). */
    gpu_display_t d;
    gpu_display_state(&d);
    int keep = fresh && d.enabled && d.w > 0 && d.h > 0;
    if (d.w > 1024) d.w = 1024;
    if (d.h > 512) d.h = 512;
    for (int y = 0; y < 512; y++) {
        u16 *dst = disp_vram + y * 1024;
        const u16 *src = game_vram + y * 1024;
        int in_rows = keep && ((y - d.y) & 511) < d.h;
        if (!in_rows) { memcpy(dst, src, 1024 * 2); continue; }
        int x0 = d.x & 1023, w = d.rgb24 ? (d.w * 3 + 1) / 2 : d.w;
        for (int x = 0; x < 1024; x++)
            if (((x - x0) & 1023) >= w) dst[x] = src[x];
    }
    disp_field = ac3_vblank_count();
    if (coop_debug > 0 && G.views < 4) printf("coop: guest picture drawn into the display VRAM at field %u\n", disp_field);
    return disp_vram;
}

u16 *ac3_coop_display_vram(u16 *game_vram) {
    /* the guest's picture is shown while it is fresh (the game draws one
     * picture per 1 or 2 fields); otherwise the game's own */
    if (disp_vram && ac3_coop_view && ac3_vblank_count() - disp_field <= 2u) return disp_vram;
    return game_vram;
}

/* ---- the replay ------------------------------------------------------------ */

/* Right after the player update (host and guest done): remember registers
 * and RAM. The rest of the frame runs normally for the host; the guest's view
 * replays it from here. */
void coop_view_mark(ctx_t *ctx, u32 resume_pc) {
    G.view_on = 0;
    if (!G.live) return;
    if (!G.snap_ram) G.snap_ram = (u8 *)malloc(AC3_RAM_SIZE);
    if (!G.snap_ram) return;
    memcpy(G.snap_ram, ac3_ram, AC3_RAM_SIZE);
    memcpy(G.snap_scratch, ac3_scratch, sizeof G.snap_scratch);
    copy_out(pp_addr(PP_BLOCK), G.host_block, BLOCK_SIZE);
    copy_out(SCENE + SCENE_PLANE, G.host_plane, 2);
    memcpy(G.flash_snap, G.flash, sizeof G.flash);   /* the guest's flash before this frame's effects */
    G.snap_ctx = *ctx;
    G.snap_pc = resume_pc;
    G.view_on = 1;
}

/* After the host's last drawing call: the frame's memory is now exactly the
 * host's. Replay the rest of the frame for the guest from the saved state
 * (the tick after the player update, camera, drawing), keep what the guest's
 * HUD decided (lock-on, both computers) and the picture it drew (guest's
 * computer only), then go back to the host's memory.
 * The replay runs in the snapshot itself: the game's RAM pointer (ac3_ram) is
 * pointed at it for the replay and back at the host's RAM afterwards, so the
 * host's memory is neither copied away nor copied back (the snapshot is taken
 * afresh next frame). */
static void view_replay(ctx_t *ctx) {
    if (!G.view_on) return;
    G.view_on = 0;
    u32 lo = 0, hi = 0;
    if (!draw_buffer(&lo, &hi)) return;
    static u8 host_scratch[1024];
    if (!G.keep_buf) G.keep_buf = (u8 *)malloc(PICTURE_MAX);
    if (!G.keep_buf) return;
    u64 t0 = coop_now_ns();
    u8 *host_ram = ac3_ram;
    memcpy(host_scratch, ac3_scratch, sizeof host_scratch);
    ctx_t host_ctx = *ctx;

    /* back to just after the player update, with the guest swapped in */
    ac3_ram = G.snap_ram;
    memcpy(ac3_scratch, G.snap_scratch, sizeof G.snap_scratch);
    *ctx = G.snap_ctx;
    u32 slots = MEM_R32(SLOT_TABLE), w = MEM_R32(WORLD_PTR);
    for (unsigned i = 0; i < N_PER_PLAYER; i++) copy_in(pp_addr(i), G.mine[i], coop_per_player[i].len);
    u32 host = slot_entity(slots, 0), mine = slot_entity(slots, 1);
    MEM_W32(slots, mine);
    MEM_W32(slots + 4, host);
    if (G.cam_valid)
        for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) copy_in(w + coop_view_private[i].addr, G.cam[i], coop_view_private[i].len);
    coop_hud_swap();          /* the guest's lock-on progress in (this RAM is thrown away after) */
    coop_threat_swap();       /* the guest's incoming missiles as "the player's" */
    if (G.shake_valid) copy_in(w + WORLD_SHAKE, G.shake, 8);   /* the guest's screen shake */
    coop_plane_object_in();   /* the guest's plane model (this RAM is thrown away after) */
    /* The camera follows "the player" through pointers into the host's entity
     * (camera object +0x34 -> its position): point them at the guest's entity. */
    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++)
        for (u32 o = 0; o + 4 <= coop_view_private[i].len; o += 4) {
            u32 a = w + coop_view_private[i].addr + o, v = MEM_R32(a);
            if (v >= host && v < host + ENT_SIZE) MEM_W32(a, mine + (v - host));
        }
    coop_prof_add(PROF_REPLAY_COPY, t0);

    /* the rest of the tick (it returns to the interpreter's sentinel, because
     * the flight frame calls it through a pointer), then the flight frame
     * from just after that call to the end of drawing */
    G.replaying = 1;
    u64 t1 = coop_now_ns();
    ac3_interpret_until(ctx, G.snap_pc, 0);
    coop_prof_add(PROF_REPLAY_TICK, t1);
    u64 t2 = coop_now_ns();
    ctx->r[31] = RA_AFTER_TICK;
    ac3_interpret_until(ctx, RA_AFTER_TICK, RA_DRAW_LAST);
    coop_prof_add(PROF_REPLAY_DRAW, t2);
    G.replaying = 0;
    u64 t3 = coop_now_ns();

    for (unsigned i = 0; i < N_VIEW_PRIVATE; i++) copy_out(w + coop_view_private[i].addr, G.cam[i], coop_view_private[i].len);
    G.cam_valid = 1;
    coop_hud_swap();          /* ... and out, with this frame's progress */
    /* The HUD code decides lock-on while it draws (0x8003F2CC writes the
     * locked target to TARGETING_STATE+0x12, plus +0x08..0x11); the next
     * frame's player update and targeting read it. So the replay runs on both
     * computers and these bytes become part of the guest's state. */
    for (u32 o = 0x08u; o <= 0x12u; o++) G.mine[PP_TARGETING][o] = (u8)MEM_R8U(TARGETING_STATE + o);
    int same = 0;
    if (ac3_coop_view) {                        /* guest's computer: keep the picture */
        u32 glo = 0, ghi = 0;
        same = draw_buffer(&glo, &ghi) && glo == lo && ghi == hi;
        if (same) {
            memcpy(G.keep_buf, ac3_ram + (lo & AC3_RAM_MASK), hi - lo);
            u32 w2 = MEM_R32(WORLD_PTR), ot0 = MEM_R32(w2 + WORLD_OT0), pk0 = MEM_R32(w2 + WORLD_PACKETS0);
            collect_packets(lo, hi, lo, (pk0 - ot0) / 4u);   /* both buffers have the same OT size */
        }
    }

    /* the host's memory and registers again */
    ac3_ram = host_ram;
    memcpy(ac3_scratch, host_scratch, sizeof host_scratch);
    *ctx = host_ctx;
    coop_prof_add(PROF_REPLAY_COPY, t3);
    G.views++;
    coop_message_log();
    if (!ac3_coop_view) return;
    if (!ov_backup) ov_backup = (u8 *)malloc(PICTURE_MAX);
    OV.ready = same && ov_backup;
    OV.lo = lo; OV.hi = hi;
    if (coop_debug > 0 && G.views < 4)
        printf("coop: guest view drawn at field %u (buffer %08X..%08X%s, %u packets outside it)\n",
               ac3_vblank_count(), lo, hi, same ? "" : ", NOT kept", n_extra);
}

/* The host's computer replays the guest's frame only for what the guest's
 * HUD decides; nobody sees that picture. Its 3D drawing (about 40% of the
 * replay) is skipped there: none of it is kept, and the HUD's decisions
 * come out the same (checked by comparing both computers' memory). */
int coop_view_skip_3d(u32 target) {
    if (!G.replaying || ac3_coop_view) return 0;
    return (target == FN_DRAW_TERRAIN && ac3_guest_ra == RA_DRAW_TERRAIN) ||
           (target == FN_DRAW_WORLD && ac3_guest_ra == RA_DRAW_WORLD);
}

/* The flight frame's last drawing call: run it, then the guest's replay. */
int coop_view_after_drawing(ctx_t *ctx) {
    if (G.replaying || ac3_guest_ra != RA_DRAW_LAST || !G.view_on) return 0;
    ac3_interpret(ctx, FN_DRAW_LAST);
    view_replay(ctx);
    return 1;
}
