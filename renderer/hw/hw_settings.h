#ifndef AC3_HW_SETTINGS_H
#define AC3_HW_SETTINGS_H

#define HW_MAX_SCALE 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int scale;
    int widescreen;
    int perspective;
    int pgxp;
    int fx;
    int far_terrain;
    int ocean;
    int clouds;
    int forest;
    int atmosphere;
    int gi;
    int sun;
    int game_fog;          /* keep the game's own fog distance */
} ac3_hd_settings_t;

extern ac3_hd_settings_t ac3_hd;
extern const ac3_hd_settings_t ac3_hd_defaults;

typedef struct {
    float volume;
    int frame_limit;
    int vsync;
    int pause_on_menu;
    int fullscreen;
    int show_stats;
    float ui_scale;
} ac3_ui_settings_t;

extern ac3_ui_settings_t ac3_ui;
extern const ac3_ui_settings_t ac3_ui_defaults;

void hw_settings_load(void);
void hw_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif
