#include "xa_adpcm.h"
#include "spu_core.h"
#include <string.h>
#include "ac3_oracle.h"

#define SUB_FILE    4
#define SUB_CHAN    5
#define SUB_MODE    6
#define SUB_CODING  7
#define SUBMODE_AUDIO 0x04
#define SUBMODE_DATA  0x08

#define USER_DATA   12
#define XA_AUDIO_BYTES 2304
#define XA_GROUPS   18

#define CODING_STEREO(c)  (((c) & 0x03) == 1)
#define CODING_18K(c)     ((((c) >> 2) & 0x03) == 1)
#define CODING_8BIT(c)    ((((c) >> 4) & 0x03) == 1)
#define CODING_EMPHASIS(c) (((c) >> 6) & 1)

static const int xa_f0[4] = { 0, 60, 115, 98 };
static const int xa_f1[4] = { 0, 0, -52, -55 };

static int filter_active;
static u8  filter_file, filter_chan;
static int rt_enabled;
static int xa_muted;

static s32 hist[2][2];

static s32 res_pos;
static s16 res_last[2];
static int res_have_last;
static u32 res_rate;

static u32 sectors_played;
static u32 sectors_seen;
static u32 sectors_filtered;
static u32 xa_peak;

void ac3_xa_reset(void) {
    memset(hist, 0, sizeof hist);
    res_pos = 0;
    res_last[0] = res_last[1] = 0;
    res_have_last = 0;
    res_rate = 0;
    filter_active = 0;
    filter_file = filter_chan = 0;
    rt_enabled = 0;
    xa_muted = 0;
    sectors_played = sectors_seen = sectors_filtered = 0;
    xa_peak = 0;
    ac3_spu_cd_flush();
}

void ac3_xa_set_filter(int active, u8 file, u8 chan) {
    if (active && (!filter_active || file != filter_file || chan != filter_chan)) {
        memset(hist, 0, sizeof hist);
        res_have_last = 0;
        res_pos = 0;
    }
    filter_active = active;
    filter_file = file;
    filter_chan = chan;
}

void ac3_xa_set_enabled(int on) {
    if (rt_enabled && !on) ac3_xa_stop();
    rt_enabled = on;
}
int ac3_xa_get_enabled(void) { return rt_enabled; }
void ac3_xa_set_muted(int on) { xa_muted = on; }

void ac3_xa_stop(void) {
    memset(hist, 0, sizeof hist);
    res_have_last = 0;
    res_pos = 0;
    ac3_spu_cd_flush();
}

static s32 clamp16s(s32 v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

#define XA_MAX_SAMPLES 4032
static s16 pcm[2][XA_MAX_SAMPLES];

static void resample_push(const s16 *l, const s16 *r, int n, u32 src_rate) {
    if (src_rate != res_rate) { res_rate = src_rate; res_pos = 0; res_have_last = 0; }

    s16 out[512 * 2];
    int fill = 0;

    for (int i = 0; i < n; i++) {
        s16 cl = l[i], cr = r[i];
        if (!res_have_last) { res_last[0] = cl; res_last[1] = cr; res_have_last = 1; }
        while (res_pos < (s32)SPU_SAMPLE_RATE) {
            s32 num = res_pos;
            s32 a = ((s32)res_last[0] * (SPU_SAMPLE_RATE - num) +
                     (s32)cl * num) / SPU_SAMPLE_RATE;
            s32 b = ((s32)res_last[1] * (SPU_SAMPLE_RATE - num) +
                     (s32)cr * num) / SPU_SAMPLE_RATE;
            if (xa_muted) { a = 0; b = 0; }
            out[fill * 2 + 0] = (s16)clamp16s(a);
            out[fill * 2 + 1] = (s16)clamp16s(b);
            {
                u32 m = (u32)(a < 0 ? -a : a);
                if (m > xa_peak) xa_peak = m;
                m = (u32)(b < 0 ? -b : b);
                if (m > xa_peak) xa_peak = m;
            }
            if (++fill == 512) { ac3_spu_cd_push(out, fill); fill = 0; }
            res_pos += (s32)src_rate;
        }
        res_pos -= (s32)SPU_SAMPLE_RATE;
        res_last[0] = cl;
        res_last[1] = cr;
    }
    if (fill) ac3_spu_cd_push(out, fill);
}

int ac3_xa_sector(const u8 *sector) {
    if (!sector) return 0;
    u8 submode = sector[SUB_MODE];
    if (!(submode & SUBMODE_AUDIO) || (submode & SUBMODE_DATA)) return 0;

    sectors_seen++;

    if (!rt_enabled) return 1;

    if (filter_active &&
        (sector[SUB_FILE] != filter_file || sector[SUB_CHAN] != filter_chan)) {
        sectors_filtered++;
        return 1;
    }

    u8 coding = sector[SUB_CODING];
    int stereo = CODING_STEREO(coding);
    int eight  = CODING_8BIT(coding);
    u32 rate = CODING_18K(coding) ? 18900u : 37800u;

    const u8 *audio = sector + USER_DATA;
    int n[2] = { 0, 0 };

    for (int g = 0; g < XA_GROUPS; g++) {
        const u8 *blk = audio + g * 128;
        const u8 *par = blk;
        const u8 *dat = blk + 16;

        if (!eight) {
            for (int u = 0; u < 8; u++) {
                u8 p = par[4 + u];
                int shift = p & 0x0F;
                int filt = (p >> 4) & 0x03;
                int ch = stereo ? (u & 1) : 0;
                s32 *h = hist[ch];
                for (int s = 0; s < 28; s++) {
                    int byte = dat[s * 4 + (u >> 1)];
                    int nib = (u & 1) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
                    s32 t = (s32)(s16)(u16)(nib << 12);
                    s32 v = (t >> shift) +
                            ((xa_f0[filt] * h[0] + xa_f1[filt] * h[1] + 32) >> 6);
                    v = clamp16s(v);
                    h[1] = h[0];
                    h[0] = v;
                    if (n[ch] < XA_MAX_SAMPLES) pcm[ch][n[ch]++] = (s16)v;
                }
            }
        } else {
            for (int u = 0; u < 4; u++) {
                u8 p = par[4 + u];
                int shift = p & 0x0F;
                int filt = (p >> 4) & 0x03;
                int ch = stereo ? (u & 1) : 0;
                s32 *h = hist[ch];
                for (int s = 0; s < 28; s++) {
                    s32 t = (s32)(s16)(u16)((u32)dat[s * 4 + u] << 8);
                    s32 v = (t >> shift) +
                            ((xa_f0[filt] * h[0] + xa_f1[filt] * h[1] + 32) >> 6);
                    v = clamp16s(v);
                    h[1] = h[0];
                    h[0] = v;
                    if (n[ch] < XA_MAX_SAMPLES) pcm[ch][n[ch]++] = (s16)v;
                }
            }
        }
    }

    sectors_played++;
    if (stereo) {
        int frames = n[0] < n[1] ? n[0] : n[1];
        resample_push(pcm[0], pcm[1], frames, rate);
    } else {
        resample_push(pcm[0], pcm[0], n[0], rate);
    }
    return 1;
}

u32 ac3_xa_sectors_played(void) { return sectors_played; }
u32 ac3_xa_peak(void) { return xa_peak; }

void ac3_xa_stats_dump(void) {
    if (!sectors_seen) return;
    printf("\nCD-XA audio: %u audio sectors seen, %u decoded, %u filtered out\n",
           sectors_seen, sectors_played, sectors_filtered);
    printf("  ADPCM playback (CdlModeRT) %s, filter %s",
           rt_enabled ? "on" : "off", filter_active ? "" : "not set");
    if (filter_active) printf("file=%u chan=%u", filter_file, filter_chan);
    printf(", peak %u/32767\n", xa_peak);
}

void ac3_xa_machine_save(FILE *f) {
    ac3_ms_tag(f, "XA");
    ac3_ms_u32(f, (u32)filter_active);
    ac3_ms_u32(f, filter_file);
    ac3_ms_u32(f, filter_chan);
    ac3_ms_u32(f, (u32)rt_enabled);
    ac3_ms_u32(f, (u32)xa_muted);
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)hist[c][i]);
    ac3_ms_u32(f, (u32)res_pos);
    for (int i = 0; i < 2; i++) ac3_ms_u32(f, (u32)(s32)res_last[i]);
    ac3_ms_u32(f, (u32)res_have_last);
    ac3_ms_u32(f, res_rate);
}
