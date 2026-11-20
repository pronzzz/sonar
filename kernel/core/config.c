/*
 * System configuration: "key = value" lines in /etc/sonar.conf ('#' starts
 * a comment). Kept in memory; config_save() writes the file back.
 */
#include "config.h"
#include "fs.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

#define MAX_KEYS 64
static struct {
    char key[32];
    char val[128];
} items[MAX_KEYS];
static int nitems;

static char *trim(char *s)
{
    while (isspace((u8)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((u8)e[-1]))
        *--e = 0;
    return s;
}

bool config_load(void)
{
    nitems = 0;
    char *data;
    size_t len;
    if (fs_read_all(CONFIG_PATH, &data, &len) < 0)
        return false;
    char *line = data;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = trim(line), *v = trim(eq + 1);
            if (*k)
                config_set(k, v);
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    return true;
}

bool config_save(void)
{
    size_t cap = 512 + (size_t)nitems * 170, n = 0;
    char *buf = kmalloc(cap);
    if (!buf)
        return false;
    n += (size_t)snprintf(buf + n, cap - n,
                          "# Sonar system configuration (edit with: config KEY VALUE)\n");
    for (int i = 0; i < nitems; i++)
        n += (size_t)snprintf(buf + n, cap - n, "%s = %s\n", items[i].key, items[i].val);
    int r = fs_write_all(CONFIG_PATH, buf, n);
    kfree(buf);
    return r == FS_OK;
}

const char *config_get(const char *key, const char *def)
{
    for (int i = 0; i < nitems; i++)
        if (!strcmp(items[i].key, key))
            return items[i].val;
    return def;
}

long config_get_int(const char *key, long def)
{
    const char *v = config_get(key, NULL);
    if (!v || !*v)
        return def;
    char *end;
    long x = strtol(v, &end, 10);
    return *end ? def : x;
}

bool config_get_bool(const char *key, bool def)
{
    const char *v = config_get(key, NULL);
    if (!v)
        return def;
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1"))
        return true;
    if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0"))
        return false;
    return def;
}

void config_set(const char *key, const char *val)
{
    for (int i = 0; i < nitems; i++) {
        if (!strcmp(items[i].key, key)) {
            strlcpy(items[i].val, val, sizeof(items[i].val));
            return;
        }
    }
    if (nitems < MAX_KEYS) {
        strlcpy(items[nitems].key, key, sizeof(items[nitems].key));
        strlcpy(items[nitems].val, val, sizeof(items[nitems].val));
        nitems++;
    }
}

static int cmd_config(int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < nitems; i++)
            printk("%-20s %s\n", items[i].key, items[i].val);
        if (!nitems)
            printk("(no settings)\n");
        return 0;
    }
    if (argc == 2) {
        const char *v = config_get(argv[1], NULL);
        printk("%s\n", v ? v : "(not set)");
        return v ? 0 : 1;
    }
    char val[128];
    size_t n = 0;
    val[0] = 0;
    for (int i = 2; i < argc; i++)
        n += (size_t)snprintf(val + n, sizeof(val) - n, i > 2 ? " %s" : "%s", argv[i]);
    config_set(argv[1], val);
    if (!config_save()) {
        printk("config: could not write %s\n", CONFIG_PATH);
        return 1;
    }
    return 0;
}
SHELL_COMMAND(config, "config", "config [KEY [VALUE]]", "show or change system settings",
              cmd_config);
