/* coop_name.c -- the other player's username beside their box on screen.
 *
 * The game labels an ally with the pilot's name to the right of the box's top
 * corner (0x80040900: x = right + 2, y = top - 4) and FRND under it. Those
 * names are pictures in the HUD's texture, one per character of the story,
 * so a player's name is drawn here instead: a small font in the same style
 * (6 pixels high, as the HUD's lettering), green and added onto the picture
 * like the game's own ally names, as one-pixel-high flat rectangles.
 *
 * Where: coop_hud.c notes the spot for each view as it draws the FRND label;
 * the name is drawn at the end of that view's pass through the ordering table
 * (psyq_gpu.c), so it lands over everything else. The host's view labels the
 * guest with the guest's name and the guest's view labels the host with the
 * host's: both computers know both names, so the host's picture (which both
 * computers draw into the game's video memory) stays the same on both. None
 * of this touches the game's memory. */
#include "coop_internal.h"
#include "gpu_internal.h"

#define NAME_MAX_CHARS  16
#define NAME_MAX_WIDTH  80     /* pixels: longer names are cut */
#define NAME_R 0x00            /* added onto the picture (the game's ally names: green) */
#define NAME_G 0x98
#define NAME_B 0x08

static char names[2][NAME_MAX_CHARS * 4 + 1];   /* [0] the host's, [1] the guest's (UTF-8 as given) */
static struct { int on; s16 x, y; } spot[2];      /* per view: 0 the host's, 1 the guest's */

/* --coop-names HOST GUEST (the page passes both, from the room server) */
void ac3_coop_set_names(const char *host, const char *guest) {
    snprintf(names[0], sizeof names[0], "%s", host ? host : "");
    snprintf(names[1], sizeof names[1], "%s", guest ? guest : "");
}

/* (coop_hud.c) the box of the other player in this view: name at its top right */
void coop_name_place(int view, s32 right, s32 top) {
    view = view ? 1 : 0;
    spot[view].on = 1;
    spot[view].x = (s16)(right + 2);
    spot[view].y = (s16)(top - 4);
}

/* ---- the font ----------------------------------------------------------------
 * Rows top to bottom, '/' between rows, '#' lit. Six rows reach the baseline
 * (capitals and ascenders); two more for descenders. Width: the longest row. */
static const char *const glyph[95] = {
    /* ' ' */ "../../../../../..",
    /* '!' */ "#/#/#/#/./#",
    /* '"' */ "#.#/#.#/.../.../.../...",
    /* '#' */ ".#.#/####/.#.#/####/.#.#/....",
    /* '$' */ NULL, /* '%' */ NULL,
    /* '&' */ ".#../#.#./.#../#.#./#..#/.##.",
    /* '\''*/ "#/#/./././.",
    /* '(' */ ".#/#./#./#./#./.#",
    /* ')' */ "#./.#/.#/.#/.#/#.",
    /* '*' */ ".../#.#/.#./#.#/.../...",
    /* '+' */ ".../.../.#./###/.#./...",
    /* ',' */ "./././././#/#",
    /* '-' */ "../../../##/../..",
    /* '.' */ "./././././#",
    /* '/' */ "..#/..#/.#./.#./#../#..",
    /* '0' */ ".#./#.#/#.#/#.#/#.#/.#.",
    /* '1' */ ".#/##/.#/.#/.#/.#",
    /* '2' */ "##./..#/..#/.#./#../###",
    /* '3' */ "##./..#/.#./..#/..#/##.",
    /* '4' */ "#.#/#.#/#.#/###/..#/..#",
    /* '5' */ "###/#../##./..#/..#/##.",
    /* '6' */ ".##/#../##./#.#/#.#/.#.",
    /* '7' */ "###/..#/..#/.#./.#./.#.",
    /* '8' */ ".#./#.#/.#./#.#/#.#/.#.",
    /* '9' */ ".#./#.#/#.#/.##/..#/##.",
    /* ':' */ "./././#/./#",
    /* ';' */ "./././#/./#/#",
    /* '<' */ ".../..#/.#./#../.#./..#",
    /* '=' */ ".../.../###/.../###/...",
    /* '>' */ ".../#../.#./..#/.#./#..",
    /* '?' */ "##./..#/.#./.#./.../.#.",
    /* '@' */ ".##./#..#/#.##/#.##/#.../.###",
    /* 'A' */ ".##./#..#/#..#/####/#..#/#..#",
    /* 'B' */ "###./#..#/###./#..#/#..#/###.",
    /* 'C' */ ".###/#.../#.../#.../#.../.###",
    /* 'D' */ "###./#..#/#..#/#..#/#..#/###.",
    /* 'E' */ "####/#.../###./#.../#.../####",
    /* 'F' */ "####/#.../###./#.../#.../#...",
    /* 'G' */ ".###/#.../#.##/#..#/#..#/.###",
    /* 'H' */ "#..#/#..#/####/#..#/#..#/#..#",
    /* 'I' */ "###/.#./.#./.#./.#./###",
    /* 'J' */ "..##/...#/...#/...#/#..#/.##.",
    /* 'K' */ "#..#/#.#./##../#.#./#..#/#..#",
    /* 'L' */ "#.../#.../#.../#.../#.../####",
    /* 'M' */ "#...#/##.##/#.#.#/#...#/#...#/#...#",
    /* 'N' */ "#..#/##.#/#.##/#..#/#..#/#..#",
    /* 'O' */ ".##./#..#/#..#/#..#/#..#/.##.",
    /* 'P' */ "###./#..#/#..#/###./#.../#...",
    /* 'Q' */ ".##./#..#/#..#/#..#/#.#./.#.#",
    /* 'R' */ "###./#..#/#..#/###./#.#./#..#",
    /* 'S' */ ".###/#.../.##./...#/...#/###.",
    /* 'T' */ "###/.#./.#./.#./.#./.#.",
    /* 'U' */ "#..#/#..#/#..#/#..#/#..#/.##.",
    /* 'V' */ "#..#/#..#/#..#/#..#/.##./.##.",
    /* 'W' */ "#...#/#...#/#...#/#.#.#/##.##/#...#",
    /* 'X' */ "#..#/#..#/.##./.##./#..#/#..#",
    /* 'Y' */ "#.#/#.#/#.#/.#./.#./.#.",
    /* 'Z' */ "####/...#/..#./.#../#.../####",
    /* '[' */ "##/#./#./#./#./##",
    /* '\\'*/ "#../#../.#./.#./..#/..#",
    /* ']' */ "##/.#/.#/.#/.#/##",
    /* '^' */ ".#./#.#/.../.../.../...",
    /* '_' */ ".../.../.../.../.../###",
    /* '`' */ "#./.#/../../../..",
    /* 'a' */ ".../.../.##/#.#/#.#/.##",
    /* 'b' */ "#../#../##./#.#/#.#/##.",
    /* 'c' */ ".../.../.##/#../#../.##",
    /* 'd' */ "..#/..#/.##/#.#/#.#/.##",
    /* 'e' */ ".../.../.#./#.#/##./.##",
    /* 'f' */ ".##/#../###/#../#../#..",
    /* 'g' */ ".../.../.##/#.#/#.#/.##/..#/##.",
    /* 'h' */ "#../#../##./#.#/#.#/#.#",
    /* 'i' */ "./#/./#/#/#",
    /* 'j' */ "../.#/../.#/.#/.#/.#/#.",
    /* 'k' */ "#../#../#.#/##./#.#/#.#",
    /* 'l' */ "#/#/#/#/#/#",
    /* 'm' */ "...../...../##.#./#.#.#/#.#.#/#.#.#",
    /* 'n' */ ".../.../##./#.#/#.#/#.#",
    /* 'o' */ ".../.../.#./#.#/#.#/.#.",
    /* 'p' */ ".../.../##./#.#/#.#/##./#../#..",
    /* 'q' */ ".../.../.##/#.#/#.#/.##/..#/..#",
    /* 'r' */ ".../.../#.#/##./#../#..",
    /* 's' */ ".../.../.##/#../..#/##.",
    /* 't' */ ".../.#./###/.#./.#./..#",
    /* 'u' */ ".../.../#.#/#.#/#.#/.##",
    /* 'v' */ ".../.../#.#/#.#/#.#/.#.",
    /* 'w' */ "...../...../#.#.#/#.#.#/#.#.#/.#.#.",
    /* 'x' */ ".../.../#.#/.#./.#./#.#",
    /* 'y' */ ".../.../#.#/#.#/#.#/.##/..#/##.",
    /* 'z' */ ".../.../###/..#/.#./###",
    /* '{' */ ".##/.#./##./.#./.#./.##",
    /* '|' */ "#/#/#/#/#/#",
    /* '}' */ "##./.#./.##/.#./.#./##.",
    /* '~' */ "..../..../.#.#/#.#./..../....",
};

/* the glyph for one character of the name: anything outside plain ASCII
 * (accents, other scripts, emoji) is a '?', one per character */
static const char *glyph_of(u32 c) {
    if (c < 32 || c > 126 || !glyph[c - 32]) c = '?';
    return glyph[c - 32];
}

static int glyph_width(const char *g) {
    int w = 0, n = 0;
    for (;; g++) {
        if (*g == '/' || !*g) { if (n > w) w = n; n = 0; if (!*g) return w; }
        else n++;
    }
}

static void tile(s32 x, s32 y, s32 w) {
    gpu_rect_t r;
    memset(&r, 0, sizeof r);
    r.semitrans = 1;
    r.r = NAME_R; r.g = NAME_G; r.b = NAME_B;
    r.x = (s16)x; r.y = (s16)y;
    r.w = (u16)w; r.h = 1;
    draw_rect(&r);
}

/* one glyph at (x, y): each row's runs of lit pixels as one rectangle */
static void draw_glyph(const char *g, s32 x, s32 y) {
    s32 col = 0, run = -1;
    for (;; g++) {
        if (*g == '#') { if (run < 0) run = col; col++; continue; }
        if (run >= 0) { tile(x + run, y, col - run); run = -1; }
        if (!*g) return;
        if (*g == '/') { y++; col = 0; } else col++;
    }
}

/* the next character of a UTF-8 string (a stray byte counts as one) */
static u32 next_char(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = *p++;
    if (c >= 0xC0) {
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        c = 0x80;                                   /* not ASCII: drawn as '?' */
        while (more-- > 0 && (*p & 0xC0) == 0x80) p++;
    }
    *s = (const char *)p;
    return c;
}

/* (psyq_gpu.c) at the end of a view's pass: its name, if a box was labelled */
void ac3_coop_name_draw(int view) {
    view = view ? 1 : 0;
    const char *s = names[view ? 0 : 1];            /* the other player's */
    if (!spot[view].on || !*s) return;
    u16 tp = gpu_sw.texpage;
    gpu_sw.texpage = (u16)((tp & ~0x60u) | 0x20u);  /* blending: added onto the picture */
    s32 x = spot[view].x, y = spot[view].y, used = 0;
    for (int n = 0; *s && n < NAME_MAX_CHARS; n++) {
        const char *g = glyph_of(next_char(&s));
        int w = glyph_width(g);
        if (used + w > NAME_MAX_WIDTH) break;
        draw_glyph(g, x + used, y);
        used += w + 1;
    }
    gpu_sw.texpage = tp;
}

/* (psyq_gpu.c) after the frame's drawing: the spots are noted afresh each frame */
void ac3_coop_name_done(void) {
    spot[0].on = spot[1].on = 0;
}
