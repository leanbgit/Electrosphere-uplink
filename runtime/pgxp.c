#include "pgxp.h"

#ifdef AC3_ENHANCED
#include <string.h>

float ac3_ws_factor = 1.0f;
int   ac3_gte_no_depthcue = 0;
int   ac3_pgxp_enabled = 1;

#define SLOTS    65536u
#define PROBES   16

typedef struct {
    u32 key;
    u32 gen;
    ac3_pgxp_vtx_t v;
} slot_t;

static slot_t table[SLOTS];
static u32 generation = 1;

#define EDGE_MAX 2048
static struct { u32 key, gen; ac3_pgxp_vtx_t v; } edge[EDGE_MAX];
static int nedge;

#define MAX_AGE 3u

u32 ac3_pgxp_projections;
void (*ac3_pgxp_tap)(u32 key, const ac3_pgxp_vtx_t *v);

static inline u32 hash(u32 k) {
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    return k & (SLOTS - 1u);
}

static inline int live_gen(u32 gen) {
    return gen && (generation - gen) <= MAX_AGE;
}

static inline int same_point(const ac3_pgxp_vtx_t *a, const ac3_pgxp_vtx_t *b) {
    float d = (a->vx - b->vx) * (a->vx - b->vx) + (a->vy - b->vy) * (a->vy - b->vy) +
              (a->vz - b->vz) * (a->vz - b->vz);
    return d < 0.25f;
}

int ac3_pgxp_edge_key(u32 key) {
    s16 x = (s16)(key & 0xFFFFu), y = (s16)(key >> 16);
    return x <= -1024 || x >= 1023 || y <= -1024 || y >= 1023;
}

void ac3_pgxp_record(u32 key, const ac3_pgxp_vtx_t *v) {
    ac3_pgxp_projections++;
    if (ac3_pgxp_tap) ac3_pgxp_tap(key, v);
    if (!ac3_pgxp_enabled) return;
    if (ac3_pgxp_edge_key(key)) {
        for (int i = 0; i < nedge; i++)
            if (edge[i].key == key && same_point(&edge[i].v, v)) {
                edge[i].gen = generation;
                edge[i].v = *v;
                return;
            }
        if (nedge < EDGE_MAX) {
            edge[nedge].key = key;
            edge[nedge].gen = generation;
            edge[nedge].v = *v;
            nedge++;
        }
        return;
    }
    u32 h = hash(key);
    slot_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < PROBES; i++) {
        slot_t *s = &table[(h + (u32)i) & (SLOTS - 1u)];
        if (!live_gen(s->gen)) {
            if (!free_slot) free_slot = s;
            continue;
        }
        if (s->key == key && same_point(&s->v, v)) {
            s->gen = generation;
            s->v = *v;
            return;
        }
        if (!oldest || s->gen < oldest->gen) oldest = s;
    }
    slot_t *victim = free_slot ? free_slot : oldest;
    victim->key = key;
    victim->gen = generation;
    victim->v = *v;
}

int ac3_pgxp_lookup_all(u32 key, ac3_pgxp_vtx_t *out, u32 *gen, int max) {
    if (!ac3_pgxp_enabled || max <= 0) return 0;
    int n = 0;
    if (ac3_pgxp_edge_key(key)) {
        for (int i = nedge - 1; i >= 0 && n < max; i--)
            if (edge[i].key == key && live_gen(edge[i].gen)) {
                out[n] = edge[i].v;
                if (gen) gen[n] = edge[i].gen;
                n++;
            }
        return n;
    }
    u32 h = hash(key);
    for (int i = 0; i < PROBES && n < max; i++) {
        const slot_t *s = &table[(h + (u32)i) & (SLOTS - 1u)];
        if (s->key != key || !live_gen(s->gen)) continue;
        int j = n++;
        while (j > 0 && gen && gen[j - 1] < s->gen) {
            out[j] = out[j - 1];
            gen[j] = gen[j - 1];
            j--;
        }
        out[j] = s->v;
        if (gen) gen[j] = s->gen;
    }
    return n;
}

int ac3_pgxp_lookup(u32 key, ac3_pgxp_vtx_t *out) {
    ac3_pgxp_vtx_t c[8];
    u32 g[8];
    int n = ac3_pgxp_lookup_all(key, c, g, 8);
    if (!n) return 0;
    *out = c[0];
    return 1;
}

void ac3_pgxp_next_frame(void) {
    ac3_pgxp_projections = 0;
    generation++;
    if (!generation) {
        memset(table, 0, sizeof table);
        nedge = 0;
        generation = 1;
    }
    int k = 0;
    for (int i = 0; i < nedge; i++)
        if (live_gen(edge[i].gen)) edge[k++] = edge[i];
    nedge = k;
}
#endif
