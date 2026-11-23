/* The player's settings file, oranges_options.ini (hos_options.c). Thread-safe; read at the first use. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void hos_options_load(void);
const char *hos_options_path(void);
/* a setting's value into out (spaces trimmed): 0 when the file does not set it */
int hos_option(const char *key, char *out, size_t size);
int hos_option_int(const char *key, int fallback);
/* set (the file is written at once), or remove (back to the default) */
void hos_option_set(const char *key, const char *value);
void hos_option_unset(const char *key);

#ifdef __cplusplus
}
#endif
