#ifndef AC3_SETTINGS_H
#define AC3_SETTINGS_H

#define AC3_SETTINGS_FILE "ac3_settings.ini"

void        ac3_settings_load(const char *path);
int         ac3_settings_save(void);
const char *ac3_settings_get(const char *key);
int         ac3_settings_int(const char *key, int def);
float       ac3_settings_float(const char *key, float def);
void        ac3_settings_set(const char *key, const char *value);
void        ac3_settings_set_int(const char *key, int value);
void        ac3_settings_set_float(const char *key, float value);

int         ac3_lang_english(void);
void        ac3_lang_set_english(int on);
const char *ac3_lang_patch_dir(void);
void        ac3_lang_set_patch_dir(const char *dir);

#endif
