/* System configuration (/etc/sonar.conf). */
#pragma once
#include "types.h"

#define CONFIG_PATH "/etc/sonar.conf"

bool config_load(void);
bool config_save(void);
const char *config_get(const char *key, const char *def);
long config_get_int(const char *key, long def);
bool config_get_bool(const char *key, bool def);
void config_set(const char *key, const char *val);
