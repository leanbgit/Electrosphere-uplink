/* coop_sound.c -- what each player hears (docs/coop/S5_sound.md).
 *
 * The game's sounds go through a table of 24 channels in memory
 * (SOUND_CHANNELS), one per voice of the sound chip. Game code writes requests
 * into it during the frame (engine, guns, missiles, explosions, the HUD's
 * tones); once per frame the sound tick (FN_SOUND_TICK) sends the changes to
 * the chip. The chip's state is read back by the game every frame, so it must
 * stay identical on both computers: it is driven exactly as before, and what
 * the speakers play comes from a second chip, "the ear" (spu_ear.h).
 *
 * Each frame, after the real sound tick, the ear gets the table as this
 * player should hear it, and the sound tick runs a second time on it with the
 * chip calls going to the ear; then the game's memory is put back.
 *
 *   Who requested a new sound (channel's key-on bit newly set):
 *     host's player update         the host's own   (gun, missiles; engine: channels 0-2)
 *     guest's player update        the guest's own
 *     rest of the frame            the host's version of the world and the
 *                                  host's HUD; the guest's view replay runs it
 *                                  again with the guest as "the player"
 *   Host's ears: the table, without the guest's own new sounds.
 *   Guest's ears: the table with the guest's engine (channels 0-2, from the
 *     guest's update), the replay's version of every channel the replay
 *     changed, and without the host's own new sounds or the new sounds only
 *     the host's frame made (its HUD's tones).
 *   Channels 6-11 (other aircraft, placed by distance from the camera):
 *     the code that places them runs in the rest of the frame, so the replay
 *     places them for the guest's camera. Their state (PLACED_STATE and the
 *     channels) is kept for the guest between frames, like its HUD: swapped
 *     into the replay, saved after it, and after the ear's sound tick (which
 *     consumes their key-on requests).
 * With no guest aircraft (menus, briefings, spectating) the ear simply
 * follows the real chip.
 *
 * The guest's plane sounds. Each plane file brings two sound definitions
 * (numbers 84 and 85: the engine) and their samples, uploaded to the same
 * place in sound memory for every plane. A definition says where its sample
 * is; a channel only says which definition it plays. The guest's samples go
 * into the ear's memory past 512 KB (EAR_EXTRA), and the guest's
 * definitions, pointing there, are in the definition table only while the
 * guest's sounds are worked out: its player update, its view replay and the
 * ear's sound tick with the guest's ears. Then the host's are put back.
 */
#include "coop_internal.h"
#include "spu_ear.h"

#define FN_SOUND_TICK    0x8002E1F4u   /* once per frame (main loop, 0x80026208): the channel table -> the chip */
#define SOUND_CHANNELS   0x800C5830u   /* 24 channels x 0x14 bytes */
#define CHANNEL_SIZE     0x14u
#define N_CHANNELS       24
#define TABLE_SIZE       (N_CHANNELS * CHANNEL_SIZE)
#define CH_FLAGS         0x01u         /* byte: bit 0 = key on at the next sound tick */
#define CH_KEY_ON        0x01u
#define ENGINE_CHANNELS  3             /* 0..2: the player's engine (the player update writes them) */
#define PLACED_FIRST     6             /* 6..11: sounds placed by distance from the camera */
#define PLACED_END       12
#define PLACED_STATE     0x800C6630u   /* the placed sounds' state (which aircraft, distance, ...) */
#define PLACED_STATE_LEN 0xB8u
#define PLACED_CH_LEN    ((PLACED_END - PLACED_FIRST) * CHANNEL_SIZE)
#define SOUND_DEFS       0x800C5A10u   /* sound definitions, 0x14 bytes each */
#define DEF_SIZE         0x14u
#define DEF_SAMPLE       0x10u         /* word: where its sample is in sound memory */
#define BANK_FIRST       0x00u         /* a plane file's sounds (entry 3): byte, the first definition's number */
#define BANK_COUNT       0x01u         /*   byte, how many */
#define BANK_DATA        0x02u         /*   halfword: the samples' offset / 4 */
#define BANK_SPU         0x04u         /*   word: where they go in sound memory */
#define BANK_SIZE        0x08u         /*   word: their size */
#define BANK_DEFS        0x0Cu         /*   the definitions */
#define BANK_MAX_DEFS    4
#define BANK_MAX_SIZE    (EAR_SAMPLE_SPACE - EAR_EXTRA)
/* the chip library functions the sound tick calls, and the ear's versions */
static const struct { u32 fn; void (*ear)(ctx_t *); } ear_library[] = {
    { 0x800A1898u, ear_psyq_SpuSetVoiceAttr },
    { 0x800A0EF8u, ear_psyq_SpuSetKey },
    { 0x800A180Cu, ear_psyq_SpuGetAllKeysStatus },
    { 0x800A13B8u, ear_psyq_SpuSetCommonAttr },
};
#define LIBRARY_LO 0x8009F078u          /* the sound library's code (psyq_spu.c) */
#define LIBRARY_HI 0x800A2000u
/* what the sound tick writes besides the channel table (measured: --coop-wfn) */
static const coop_range_t tick_writes[] = {
    { 0x800C5800u, 0x30u },     /* the chip settings it passes to SpuSetCommonAttr */
    { 0x800BCE98u, 0x04u },
    { 0x800C6550u, 0x04u },
};
#define TICK_STACK 0x800u               /* and below its stack pointer */

int ac3_coop_ears = -1;   /* -1: by computer (the guest's if it shows the guest's view alone); 0 host's; 1 guest's */

static struct {
    int ear_pass;                         /* the sound tick is running for the ear */
    int valid;                            /* this frame's tables below were taken */
    u8 before_host[TABLE_SIZE], after_host[TABLE_SIZE], after_guest[TABLE_SIZE];
    u8 at_mark[TABLE_SIZE], after_replay[TABLE_SIZE];
    int replayed;
    u32 unknown_calls;
    /* the guest's placed sounds between frames */
    int placed_valid;
    u8 placed_state[PLACED_STATE_LEN], placed_ch[PLACED_CH_LEN];
    int was_own;                          /* the ear had the guest's own sound last tick */
} S;

/* the guest's plane sounds (none: the same plane as the host's) */
static struct {
    int count, first, uploaded;
    u8 defs[BANK_MAX_DEFS * DEF_SIZE];    /* pointing into the ear's memory */
    u8 *samples;
    u32 size;
} B;

void coop_sound_guest_bank(u32 bank) {
    B.count = 0; B.uploaded = 0;
    if (!bank) return;
    u32 first = MEM_R8U(bank + BANK_FIRST), count = MEM_R8U(bank + BANK_COUNT), size = MEM_R32(bank + BANK_SIZE);
    u32 spu = MEM_R32(bank + BANK_SPU), data = bank + 4u * MEM_R16U(bank + BANK_DATA);
    if (!count || count > BANK_MAX_DEFS || first + count > 256u || !size || size > BANK_MAX_SIZE) {
        printf("coop: sound: the guest's plane sounds look wrong (first %u, %u sounds, %u bytes): not used\n", first, count, size);
        return;
    }
    copy_out(bank + BANK_DEFS, B.defs, count * DEF_SIZE);
    for (u32 i = 0; i < count; i++) {                  /* their samples' place: in the ear's memory */
        u8 *d = B.defs + i * DEF_SIZE;
        u32 a = (u32)d[DEF_SAMPLE] | (u32)d[DEF_SAMPLE + 1] << 8 | (u32)d[DEF_SAMPLE + 2] << 16 | (u32)d[DEF_SAMPLE + 3] << 24;
        a = a - spu + EAR_EXTRA;
        for (int k = 0; k < 4; k++) d[DEF_SAMPLE + k] = (u8)(a >> (8 * k));
    }
    u8 *s = (u8 *)realloc(B.samples, size);
    if (!s) return;
    B.samples = s;
    copy_out(data, B.samples, size);
    B.size = size;
    B.first = (int)first;
    B.count = (int)count;
    printf("coop: sound: the guest's plane sounds: %u definitions from %u, %u bytes of samples\n", count, first, size);
}

/* the guest's definitions in or out of the definition table (both computers alike) */
void coop_sound_bank_swap(void) {
    if (B.count) exchange(SOUND_DEFS + (u32)B.first * DEF_SIZE, B.defs, (u32)B.count * DEF_SIZE);
}
static int guest_bank_sound(const u8 *channel) {
    return B.count && channel[0] >= B.first && channel[0] < B.first + B.count;
}

static int guest_ears(void) {
    if (ac3_coop_ears >= 0) return ac3_coop_ears;
    return ac3_coop_view && !ac3_coop_split;
}
static u8 *ch(u8 *table, int c) { return table + (u32)c * CHANNEL_SIZE; }
static int new_key_on(const u8 *before, const u8 *after, int c) {
    return (after[(u32)c * CHANNEL_SIZE + CH_FLAGS] & CH_KEY_ON) && !(before[(u32)c * CHANNEL_SIZE + CH_FLAGS] & CH_KEY_ON);
}

/* ---- this frame's tables (coop_guest.c, coop_view.c) ---------------------- */
void coop_sound_before_host(void) { copy_out(SOUND_CHANNELS, S.before_host, TABLE_SIZE); S.valid = 0; S.replayed = 0; }
void coop_sound_after_host(void)  { copy_out(SOUND_CHANNELS, S.after_host, TABLE_SIZE); }
/* after the guest's update, before the host-only memory is put back (the engine channels) */
void coop_sound_after_guest(void) { copy_out(SOUND_CHANNELS, S.after_guest, TABLE_SIZE); S.valid = 1; }
void coop_sound_mark(void)        { copy_out(SOUND_CHANNELS, S.at_mark, TABLE_SIZE); }
/* The guest's view replay (its RAM is thrown away after): in goes the
 * guest's placed-sound state, out comes the table as the guest's frame left it. */
void coop_sound_replay_in(void) {
    if (!S.placed_valid) return;                       /* the first frame: from the host's */
    copy_in(PLACED_STATE, S.placed_state, PLACED_STATE_LEN);
    copy_in(SOUND_CHANNELS + PLACED_FIRST * CHANNEL_SIZE, S.placed_ch, PLACED_CH_LEN);
}
void coop_sound_after_replay(void) {
    copy_out(SOUND_CHANNELS, S.after_replay, TABLE_SIZE);
    copy_out(PLACED_STATE, S.placed_state, PLACED_STATE_LEN);
    memcpy(S.placed_ch, ch(S.after_replay, PLACED_FIRST), PLACED_CH_LEN);
    S.placed_valid = 1;
    S.replayed = 1;
}

/* ---- the table as this player hears it -------------------------------------- */
static u32 build_table(u8 *t) {
    u32 silenced = 0;
    #define SILENCE(c) do { ch(t, c)[CH_FLAGS] &= (u8)~CH_KEY_ON; silenced |= 1u << (c); } while (0)
    if (!guest_ears()) {
        for (int c = ENGINE_CHANNELS; c < N_CHANNELS; c++)
            if (new_key_on(S.after_host, S.after_guest, c)) SILENCE(c);
        return silenced;
    }
    for (int c = 0; c < ENGINE_CHANNELS; c++) {
        memcpy(ch(t, c), ch(S.after_guest, c), CHANNEL_SIZE);
        /* the ear was playing the host's engine: start the guest's plane sound */
        if (!S.was_own && guest_bank_sound(ch(t, c))) ch(t, c)[CH_FLAGS] |= CH_KEY_ON;
    }
    for (int c = ENGINE_CHANNELS; c < N_CHANNELS; c++) {
        if (new_key_on(S.before_host, S.after_host, c)) { SILENCE(c); continue; }
        if (!S.replayed) continue;
        if (c >= PLACED_FIRST && c < PLACED_END) { memcpy(ch(t, c), ch(S.after_replay, c), CHANNEL_SIZE); silenced &= ~(1u << c); continue; }
        if (memcmp(ch(S.after_replay, c), ch(S.at_mark, c), CHANNEL_SIZE)) {
            memcpy(ch(t, c), ch(S.after_replay, c), CHANNEL_SIZE);   /* the guest's version */
            silenced &= ~(1u << c);
        } else if (new_key_on(S.at_mark, t, c)) {
            SILENCE(c);                                               /* only the host's frame made it */
        }
    }
    #undef SILENCE
    return silenced;
}

/* ---- the sound tick ------------------------------------------------------------ */
static void ear_pass(ctx_t *entry, const u8 *table) {
    u8 keep_table[TABLE_SIZE], keep[3][0x30], stack[TICK_STACK];
    u32 sp = entry->r[29];
    copy_out(SOUND_CHANNELS, keep_table, TABLE_SIZE);
    for (unsigned i = 0; i < 3; i++) copy_out(tick_writes[i].addr, keep[i], tick_writes[i].len);
    copy_out(sp - TICK_STACK, stack, TICK_STACK);

    copy_in(SOUND_CHANNELS, table, TABLE_SIZE);
    int bank = guest_ears() && B.count;
    if (bank && !B.uploaded && ear_ac3_spu_ram) {
        memcpy(ear_ac3_spu_ram + EAR_EXTRA, B.samples, B.size);
        B.uploaded = 1;
    }
    if (bank) coop_sound_bank_swap();
    ctx_t c = *entry;
    S.ear_pass = 1;
    ac3_interpret(&c, FN_SOUND_TICK);
    S.ear_pass = 0;
    if (bank) coop_sound_bank_swap();
    if (guest_ears() && S.placed_valid)   /* the requests it consumed: not again next frame */
        copy_out(SOUND_CHANNELS + PLACED_FIRST * CHANNEL_SIZE, S.placed_ch, PLACED_CH_LEN);

    copy_in(SOUND_CHANNELS, keep_table, TABLE_SIZE);
    for (unsigned i = 0; i < 3; i++) copy_in(tick_writes[i].addr, keep[i], tick_writes[i].len);
    copy_in(sp - TICK_STACK, stack, TICK_STACK);
}

static int sound_tick(ctx_t *ctx) {
    if (!ac3_spu_ear_on) { ac3_spu_ear_copy_all(); ac3_spu_ear_heard = 1; }
    ctx_t entry = *ctx;
    u8 table[TABLE_SIZE];
    copy_out(SOUND_CHANNELS, table, TABLE_SIZE);
    ac3_interpret(ctx, FN_SOUND_TICK);                 /* the real chip, as always */
    int own = G.live && S.valid;
    if (!G.live) S.placed_valid = 0;                  /* the next guest aircraft starts from the host's */
    S.valid = 0;
    ac3_spu_ear_mirror = !own;
    if (!own) { S.was_own = 0; return 1; }
    ac3_spu_ear_follow(SPU_STATE_SETTINGS);           /* volumes, reverb: the same for both */
    u8 real_table[TABLE_SIZE];
    if (coop_debug > 0) memcpy(real_table, table, TABLE_SIZE);
    u32 silenced = build_table(table);
    ear_pass(&entry, table);
    S.was_own = 1;
    if (coop_debug > 0) {            /* key-ons per channel: the real chip's, the ear's; silenced */
        static u32 quiet[N_CHANNELS], real_on[N_CHANNELS], ear_on[N_CHANNELS], ticks;
        ticks++;
        for (int c = 0; c < N_CHANNELS; c++) {
            if (silenced & (1u << c)) quiet[c]++;
            if (ch(real_table, c)[CH_FLAGS] & CH_KEY_ON) real_on[c]++;
            if (ch(table, c)[CH_FLAGS] & CH_KEY_ON) ear_on[c]++;
        }
        if (ticks % 1800u == 0) {
            printf("coop: sound: %s's ears, %u ticks; per channel key-ons real/ear (silenced):", guest_ears() ? "guest" : "host", ticks);
            for (int c = 0; c < N_CHANNELS; c++) if (real_on[c] || ear_on[c] || quiet[c]) printf(" %d:%u/%u(%u)", c, real_on[c], ear_on[c], quiet[c]);
            printf("\n");
        }
    }
    for (int c = 0; c < N_CHANNELS; c++)
        if (silenced & (1u << c)) { ear_ac3_spu_core_key_off(c); ear_ac3_spu_voice[c].keyed_on = 0; }
    return 1;
}

int coop_sound_hook(ctx_t *ctx, u32 target) {
    if (S.ear_pass) {
        for (unsigned i = 0; i < sizeof ear_library / sizeof ear_library[0]; i++)
            if (ear_library[i].fn == target) { ear_library[i].ear(ctx); return 1; }
        if (target >= LIBRARY_LO && target < LIBRARY_HI) {   /* a chip call not expected here: not to the real chip */
            if (S.unknown_calls++ < 8) printf("coop: sound: library call %08X in the ear's sound tick skipped\n", target);
            ctx->r[2] = 0;
            return 1;
        }
        return 0;
    }
    if (target == FN_SOUND_TICK && !G.replaying) return sound_tick(ctx);
    return 0;
}
