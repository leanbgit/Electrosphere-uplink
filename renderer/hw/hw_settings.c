#include "hw_settings.h"
#include "ac3_settings.h"

const ac3_hd_settings_t ac3_hd_defaults = { 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
ac3_hd_settings_t ac3_hd = { 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

const ac3_ui_settings_t ac3_ui_defaults = { 1.0f, 1, 1, 1, 0, 0, 1.0f };
ac3_ui_settings_t ac3_ui = { 1.0f, 1, 1, 1, 0, 0, 1.0f };

static const struct { const char *key; int *v; } flags[] = {
    { "widescreen",    &ac3_hd.widescreen },
    { "perspective",   &ac3_hd.perspective },
    { "pgxp",          &ac3_hd.pgxp },
    { "world_fx",      &ac3_hd.fx },
    { "far_terrain",   &ac3_hd.far_terrain },
    { "ocean",         &ac3_hd.ocean },
    { "clouds",        &ac3_hd.clouds },
    { "forest",        &ac3_hd.forest },
    { "atmosphere",    &ac3_hd.atmosphere },
    { "gi",            &ac3_hd.gi },
    { "sun",           &ac3_hd.sun },
    { "game_fog",      &ac3_hd.game_fog },
    { "frame_limit",   &ac3_ui.frame_limit },
    { "vsync",         &ac3_ui.vsync },
    { "pause_on_menu", &ac3_ui.pause_on_menu },
    { "fullscreen",    &ac3_ui.fullscreen },
    { "show_stats",    &ac3_ui.show_stats },
};

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void hw_settings_load(void) {
    int s = ac3_settings_int("scale", ac3_hd.scale);
    ac3_hd.scale = s < 1 ? 1 : s > HW_MAX_SCALE ? HW_MAX_SCALE : s;
    for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
        *flags[i].v = ac3_settings_int(flags[i].key, *flags[i].v) != 0;
    ac3_ui.volume = clampf(ac3_settings_float("volume", ac3_ui.volume), 0.0f, 1.0f);
    ac3_ui.ui_scale = clampf(ac3_settings_float("ui_scale", ac3_ui.ui_scale), 0.5f, 3.0f);
}

void hw_settings_save(void) {
    ac3_settings_set_int("scale", ac3_hd.scale);
    for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
        ac3_settings_set_int(flags[i].key, *flags[i].v);
    ac3_settings_set_float("volume", ac3_ui.volume);
    ac3_settings_set_float("ui_scale", ac3_ui.ui_scale);
    ac3_settings_save();
}
