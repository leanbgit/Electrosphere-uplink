#include "ac3_settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 96

static struct {
    char path[512];
    int n;
    char key[MAX_KEYS][48];
    char val[MAX_KEYS][464];
} S;

static int english;
static char patch_dir[512];

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

void ac3_settings_load(const char *path) {
    snprintf(S.path, sizeof S.path, "%s", path);
    S.n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        ac3_settings_set(trim(s), trim(eq + 1));
    }
    fclose(f);
}

int ac3_settings_save(void) {
    if (!S.path[0]) snprintf(S.path, sizeof S.path, "%s", AC3_SETTINGS_FILE);
    FILE *f = fopen(S.path, "w");
    if (!f) return 0;
    for (int i = 0; i < S.n; i++) fprintf(f, "%s = %s\n", S.key[i], S.val[i]);
    return fclose(f) == 0;
}

const char *ac3_settings_get(const char *key) {
    for (int i = 0; i < S.n; i++)
        if (!strcmp(S.key[i], key)) return S.val[i];
    return NULL;
}

int ac3_settings_int(const char *key, int def) {
    const char *v = ac3_settings_get(key);
    if (!v || !*v) return def;
    char *end;
    long x = strtol(v, &end, 0);
    return end == v ? def : (int)x;
}

float ac3_settings_float(const char *key, float def) {
    const char *v = ac3_settings_get(key);
    if (!v || !*v) return def;
    char *end;
    double x = strtod(v, &end);
    return end == v ? def : (float)x;
}

void ac3_settings_set(const char *key, const char *value) {
    if (!key || !*key) return;
    for (int i = 0; i < S.n; i++)
        if (!strcmp(S.key[i], key)) {
            snprintf(S.val[i], sizeof S.val[i], "%s", value ? value : "");
            return;
        }
    if (S.n == MAX_KEYS) return;
    snprintf(S.key[S.n], sizeof S.key[S.n], "%s", key);
    snprintf(S.val[S.n], sizeof S.val[S.n], "%s", value ? value : "");
    S.n++;
}

void ac3_settings_set_int(const char *key, int value) {
    char b[32];
    snprintf(b, sizeof b, "%d", value);
    ac3_settings_set(key, b);
}

void ac3_settings_set_float(const char *key, float value) {
    char b[32];
    snprintf(b, sizeof b, "%g", (double)value);
    ac3_settings_set(key, b);
}

int ac3_lang_english(void) { return english; }
void ac3_lang_set_english(int on) { english = on ? 1 : 0; }
const char *ac3_lang_patch_dir(void) { return patch_dir[0] ? patch_dir : NULL; }
void ac3_lang_set_patch_dir(const char *dir) { snprintf(patch_dir, sizeof patch_dir, "%s", dir ? dir : ""); }
