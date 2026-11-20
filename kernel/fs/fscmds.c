/* Root file system selection at boot, and the file shell commands. */
#include "console.h"
#include "fs.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

static const char *std_dirs[] = { "/etc", "/home", "/models", "/tmp", "/var", "/var/log" };

/* GPT type of SonarFS partitions (tools/sonarfs.py, tools/mkesp.py). */
static const u8 sonar_type[16] = { 0x12, 0x7a, 0x0e, 0x5a, 0xfa, 0x50, 0x53, 0x4f,
                                   0x9d, 0x3e, 0x53, 0x6f, 0x6e, 0x61, 0x72, 0x46 };

static bool try_mount(struct blkdev *d)
{
    if (d->readonly || !fs_probe(d))
        return false;
    int r = fs_mount(d, false);
    if (r != FS_OK) {
        printk("fs: %s: %s\n", d->name, fs_strerror(r));
        return false;
    }
    char label[33];
    u64 total, freeb;
    fs_usage(&total, &freeb, label, sizeof(label));
    printk("fs: mounted %s (\"%s\") as /, %llu MiB free%s\n", d->name, label,
           (unsigned long long)(freeb >> 20), d->mem ? " (in memory; changes are not saved)" : "");
    return true;
}

void fs_boot(void)
{
    /* The system disk: a SonarFS partition on a real disk (one made by
     * mkesp.py --system on the boot drive, or by the installer), then any
     * writable SonarFS volume, disks before in-memory copies. */
    for (int pass = 0; pass < 3 && !fs_mounted(); pass++)
        for (int i = 0; i < blk_count() && !fs_mounted(); i++) {
            struct blkdev *d = blk_get(i);
            bool typed = d->gpt && !memcmp(d->type_guid, sonar_type, 16);
            bool in_ram = d->mem || (d->parent && d->parent->mem);
            if ((pass == 0 && typed && !in_ram) || (pass == 1 && !in_ram) || (pass == 2 && in_ram))
                try_mount(d);
        }
    if (!fs_mounted()) {
        char err[96];
        struct blkdev *rd = ramdisk_create("ram0", 64ULL << 20);
        if (rd && fs_mkfs(rd, "RAM", err, sizeof(err)) && fs_mount(rd, true) == FS_OK)
            printk("fs: no system disk found; using a 64 MiB RAM disk (nothing is saved)\n");
        else
            printk("fs: no file system available\n");
    }
    if (fs_mounted())
        for (unsigned i = 0; i < sizeof(std_dirs) / sizeof(std_dirs[0]); i++)
            fs_mkdir(std_dirs[i]);
}

static int fail(const char *cmd, const char *what, int e)
{
    printk("%s: %s: %s\n", cmd, what, fs_strerror(e));
    return 1;
}

static void fmt_time(u64 t, char *out, size_t n)
{
    /* civil-from-days (Howard Hinnant) */
    u64 days = t / 86400, secs = t % 86400;
    i64 z = (i64)days + 719468, era = z / 146097;
    u64 doe = (u64)(z - era * 146097);
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u64 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, n, "%04lld-%02llu-%02llu %02llu:%02llu", (long long)y, (unsigned long long)m,
             (unsigned long long)d, (unsigned long long)(secs / 3600),
             (unsigned long long)(secs / 60 % 60));
}

static int cmd_ls(int argc, char **argv)
{
    bool lng = false;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        lng = true;
        first = 2;
    }
    const char *path = argc > first ? argv[first] : ".";
    struct fs_stat st;
    int r = fs_stat(path, &st);
    if (r < 0)
        return fail("ls", path, r);
    if (st.type != FT_DIR) {
        printk("%s\n", path);
        return 0;
    }
    struct fs_dirent *ents;
    int n;
    if ((r = fs_list(path, &ents, &n)) < 0)
        return fail("ls", path, r);
    char abs[FS_PATH_MAX];
    fs_abspath(path, abs, sizeof(abs));
    for (int i = 0; i < n; i++) {
        bool dir = ents[i].type == FT_DIR;
        const char *c0 = dir && !console_redirected() ? "\x1b[1;34m" : "";
        const char *c1 = dir ? (console_redirected() ? "/" : "/\x1b[0m") : "";
        if (lng) {
            char full[FS_PATH_MAX + FS_NAME_MAX + 2], sz[24] = "-", tm[24] = "";
            snprintf(full, sizeof(full), "%s/%s", strcmp(abs, "/") ? abs : "", ents[i].name);
            struct fs_stat es;
            if (fs_stat(full, &es) == FS_OK) {
                if (!dir)
                    blk_format_size(es.size, sz, sizeof(sz));
                fmt_time(es.mtime, tm, sizeof(tm));
            }
            printk("%s %11s  %s  %s%s%s\n", dir ? "d" : "-", sz, tm, c0, ents[i].name, c1);
        } else {
            printk("%s%s%s\n", c0, ents[i].name, c1);
        }
    }
    kfree(ents);
    return 0;
}
SHELL_COMMAND(ls, "ls", "ls [-l] [PATH]", "list a directory", cmd_ls);

static int cmd_cd(int argc, char **argv)
{
    int r = fs_chdir(argc > 1 ? argv[1] : "/");
    return r < 0 ? fail("cd", argc > 1 ? argv[1] : "/", r) : 0;
}
SHELL_COMMAND(cd, "cd", "cd [DIR]", "change the current directory", cmd_cd);

static int cmd_pwd(int argc, char **argv)
{
    printk("%s\n", fs_cwd());
    return 0;
}
SHELL_COMMAND(pwd, "pwd", "pwd", "print the current directory", cmd_pwd);

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: cat FILE...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = fs_open(argv[i], O_READ);
        if (fd < 0) {
            rc = fail("cat", argv[i], fd);
            continue;
        }
        char buf[512];
        ssize_t n;
        while ((n = fs_read(fd, buf, sizeof(buf))) > 0)
            console_write(buf, (size_t)n);
        fs_close(fd);
        if (n < 0)
            rc = fail("cat", argv[i], (int)n);
    }
    return rc;
}
SHELL_COMMAND(cat, "cat", "cat FILE...", "print files", cmd_cat);

static int cmd_head(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: head [-n N] FILE\n");
        return 1;
    }
    int lines = 10, i = 1;
    if (argc > 3 && !strcmp(argv[1], "-n")) {
        lines = (int)strtol(argv[2], NULL, 10);
        i = 3;
    }
    char *data;
    size_t len;
    int r = fs_read_all(argv[i], &data, &len);
    if (r < 0)
        return fail("head", argv[i], r);
    size_t end = 0;
    for (int l = 0; end < len && l < lines; end++)
        if (data[end] == '\n')
            l++;
    console_write(data, end);
    kfree(data);
    return 0;
}
SHELL_COMMAND(head, "head", "head [-n N] FILE", "print the first lines of a file", cmd_head);

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: mkdir [-p] DIR...\n");
        return 1;
    }
    bool parents = !strcmp(argv[1], "-p");
    int rc = 0;
    for (int i = parents ? 2 : 1; i < argc; i++) {
        if (parents) {
            char abs[FS_PATH_MAX];
            if (fs_abspath(argv[i], abs, sizeof(abs)) < 0)
                continue;
            for (char *p = abs + 1;; p++) {
                if (*p == '/' || !*p) {
                    char c = *p;
                    *p = 0;
                    int r = fs_mkdir(abs);
                    if (r < 0 && r != FS_EEXIST) {
                        rc = fail("mkdir", abs, r);
                        break;
                    }
                    *p = c;
                    if (!c)
                        break;
                }
            }
        } else {
            int r = fs_mkdir(argv[i]);
            if (r < 0)
                rc = fail("mkdir", argv[i], r);
        }
    }
    return rc;
}
SHELL_COMMAND(mkdir, "mkdir", "mkdir [-p] DIR...", "create directories", cmd_mkdir);

static int rm_tree(const char *path)
{
    struct fs_stat st;
    int r = fs_stat(path, &st);
    if (r < 0)
        return r;
    if (st.type != FT_DIR)
        return fs_unlink(path);
    struct fs_dirent *ents;
    int n;
    if ((r = fs_list(path, &ents, &n)) < 0)
        return r;
    char abs[FS_PATH_MAX];
    fs_abspath(path, abs, sizeof(abs));
    for (int i = 0; i < n && r == FS_OK; i++) {
        char *child = kmalloc(FS_PATH_MAX + FS_NAME_MAX + 2);
        snprintf(child, FS_PATH_MAX + FS_NAME_MAX + 2, "%s/%s", abs, ents[i].name);
        r = rm_tree(child);
        kfree(child);
    }
    kfree(ents);
    return r < 0 ? r : fs_rmdir(path);
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: rm [-r] PATH...\n");
        return 1;
    }
    bool rec = !strcmp(argv[1], "-r") || !strcmp(argv[1], "-rf");
    int rc = 0;
    for (int i = rec ? 2 : 1; i < argc; i++) {
        char abs[FS_PATH_MAX];
        if (fs_abspath(argv[i], abs, sizeof(abs)) == FS_OK && !strcmp(abs, "/")) {
            printk("rm: refusing to remove /\n");
            rc = 1;
            continue;
        }
        int r = rec ? rm_tree(argv[i]) : fs_unlink(argv[i]);
        if (r < 0)
            rc = fail("rm", argv[i], r);
    }
    return rc;
}
SHELL_COMMAND(rm, "rm", "rm [-r] PATH...", "remove files (-r: directories too)", cmd_rm);

static int cmd_rmdir(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = fs_rmdir(argv[i]);
        if (r < 0)
            rc = fail("rmdir", argv[i], r);
    }
    return rc;
}
SHELL_COMMAND(rmdir, "rmdir", "rmdir DIR...", "remove empty directories", cmd_rmdir);

static int cmd_mv(int argc, char **argv)
{
    if (argc != 3) {
        printk("usage: mv FROM TO\n");
        return 1;
    }
    int r = fs_rename(argv[1], argv[2]);
    return r < 0 ? fail("mv", argv[1], r) : 0;
}
SHELL_COMMAND(mv, "mv", "mv FROM TO", "move or rename", cmd_mv);

static int cmd_cp(int argc, char **argv)
{
    if (argc != 3) {
        printk("usage: cp FROM TO\n");
        return 1;
    }
    char dst[FS_PATH_MAX + FS_NAME_MAX + 2];
    strlcpy(dst, argv[2], sizeof(dst));
    struct fs_stat st;
    if (fs_stat(argv[2], &st) == FS_OK && st.type == FT_DIR) {
        const char *base = strrchr(argv[1], '/');
        snprintf(dst, sizeof(dst), "%s/%s", argv[2], base ? base + 1 : argv[1]);
    }
    int in = fs_open(argv[1], O_READ);
    if (in < 0)
        return fail("cp", argv[1], in);
    int out = fs_open(dst, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0) {
        fs_close(in);
        return fail("cp", dst, out);
    }
    const size_t chunk = 1 << 20;
    u8 *buf = kmalloc(chunk);
    ssize_t n = buf ? 0 : FS_ENOMEM;
    while (buf && (n = fs_read(in, buf, chunk)) > 0) {
        ssize_t w = fs_write(out, buf, (size_t)n);
        if (w < 0) {
            n = w;
            break;
        }
    }
    kfree(buf);
    fs_close(in);
    fs_close(out);
    return n < 0 ? fail("cp", dst, (int)n) : 0;
}
SHELL_COMMAND(cp, "cp", "cp FROM TO", "copy a file", cmd_cp);

static int cmd_touch(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = fs_open(argv[i], O_WRITE | O_CREATE);
        if (fd < 0)
            rc = fail("touch", argv[i], fd);
        else
            fs_close(fd);
    }
    return rc;
}
SHELL_COMMAND(touch, "touch", "touch FILE...", "create empty files", cmd_touch);

static int write_text(int argc, char **argv, bool append)
{
    if (argc < 2) {
        printk("usage: %s FILE [TEXT...]\n", argv[0]);
        return 1;
    }
    char *text = kmalloc(4096);
    size_t n = 0;
    for (int i = 2; i < argc; i++)
        n += (size_t)snprintf(text + n, 4096 - n, i > 2 ? " %s" : "%s", argv[i]);
    if (n < 4095)
        text[n++] = '\n';
    if (argc == 2)
        n = 0;
    /* interpret \n escapes so multi-line text can be written in one command */
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        if (text[r] == '\\' && r + 1 < n && text[r + 1] == 'n') {
            text[w++] = '\n';
            r++;
        } else {
            text[w++] = text[r];
        }
    }
    if (append && w) { /* the line starts on a line of its own */
        int fd = fs_open(argv[1], O_READ);
        if (fd >= 0) {
            u64 size = fs_fsize(fd);
            char last = '\n';
            if (size)
                fs_pread(fd, size - 1, &last, 1);
            fs_close(fd);
            if (last != '\n' && w < 4096) {
                memmove(text + 1, text, w);
                text[0] = '\n';
                w++;
            }
        }
    }
    int rc = append ? fs_append(argv[1], text, w) : fs_write_all(argv[1], text, w);
    kfree(text);
    return rc < 0 ? fail(argv[0], argv[1], rc) : 0;
}

static int cmd_write(int argc, char **argv) { return write_text(argc, argv, false); }
static int cmd_append(int argc, char **argv) { return write_text(argc, argv, true); }
SHELL_COMMAND(write, "write", "write FILE TEXT...", "write a line of text to a file (\\n = newline)",
              cmd_write);
SHELL_COMMAND(append, "append", "append FILE TEXT...", "append a line of text to a file",
              cmd_append);

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: stat PATH\n");
        return 1;
    }
    struct fs_stat st;
    int r = fs_stat(argv[1], &st);
    if (r < 0)
        return fail("stat", argv[1], r);
    char c[24], m[24];
    fmt_time(st.ctime, c, sizeof(c));
    fmt_time(st.mtime, m, sizeof(m));
    printk("  path: %s\n  type: %s\n  size: %llu bytes\n inode: %u  links: %u  extents: %u\n"
           "created: %s UTC\nmodified: %s UTC\n",
           argv[1], st.type == FT_DIR ? "directory" : "file", (unsigned long long)st.size, st.ino,
           st.links, st.nextents, c, m);
    return 0;
}
SHELL_COMMAND(stat, "stat", "stat PATH", "show file details", cmd_stat);

static int cmd_df(int argc, char **argv)
{
    if (!fs_mounted()) {
        printk("df: no file system mounted\n");
        return 1;
    }
    u64 total, freeb;
    char label[33], t[24], u[24], f[24];
    fs_usage(&total, &freeb, label, sizeof(label));
    blk_format_size(total, t, sizeof(t));
    blk_format_size(total - freeb, u, sizeof(u));
    blk_format_size(freeb, f, sizeof(f));
    printk("%-8s %-10s %11s %11s %11s %4s\n", "DEVICE", "LABEL", "SIZE", "USED", "FREE", "USE%");
    printk("%-8s %-10s %11s %11s %11s %3llu%%%s\n", fs_device(), label, t, u, f,
           (unsigned long long)(total ? (total - freeb) * 100 / total : 0),
           fs_is_ram() ? "  (RAM: not saved)" : "");
    return 0;
}
SHELL_COMMAND(df, "df", "df", "show file system usage", cmd_df);

static int cmd_mkfs(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: mkfs DEVICE [LABEL]   (erases the device!)\n");
        return 1;
    }
    struct blkdev *d = blk_find(argv[1]);
    if (!d) {
        printk("mkfs: no such device: %s\n", argv[1]);
        return 1;
    }
    if (fs_mounted() && !strcmp(fs_device(), d->name)) {
        printk("mkfs: %s is mounted as /\n", d->name);
        return 1;
    }
    char err[96];
    if (!fs_mkfs(d, argc > 2 ? argv[2] : "SONAR", err, sizeof(err))) {
        printk("mkfs: %s\n", err);
        return 1;
    }
    printk("mkfs: created SonarFS on %s\n", d->name);
    return 0;
}
SHELL_COMMAND(mkfs, "mkfs", "mkfs DEVICE [LABEL]", "format a device with SonarFS (erases it)",
              cmd_mkfs);

static int cmd_mount(int argc, char **argv)
{
    if (argc < 2) {
        printk("/ is %s%s\n", fs_device(), fs_is_ram() ? " (RAM disk)" : "");
        return 0;
    }
    struct blkdev *d = blk_find(argv[1]);
    if (!d) {
        printk("mount: no such device: %s\n", argv[1]);
        return 1;
    }
    int r = fs_mount(d, false);
    if (r < 0)
        return fail("mount", argv[1], r);
    for (unsigned i = 0; i < sizeof(std_dirs) / sizeof(std_dirs[0]); i++)
        fs_mkdir(std_dirs[i]);
    printk("mounted %s as /\n", d->name);
    return 0;
}
SHELL_COMMAND(mount, "mount", "mount [DEVICE]", "show or change the root file system", cmd_mount);

/* Journal test: write a file, then cut the power right after its metadata
 * is committed to the journal; the next mount must finish the update. */
static int cmd_jcrash(int argc, char **argv)
{
    if (argc < 3) {
        printk("usage: jcrash FILE TEXT [N]   (writes FILE, powers off at its Nth commit, default 2)\n");
        return 1;
    }
    extern int fs_crash_test;
    fs_crash_test = argc > 3 ? (int)strtol(argv[3], NULL, 10) : 2;
    int r = fs_write_all(argv[1], argv[2], strlen(argv[2]));
    fs_crash_test = 0;
    printk("jcrash: the volume has no journal (%s)\n", fs_strerror(r));
    return 1;
}
SHELL_COMMAND(jcrash, "jcrash", "jcrash FILE TEXT [N]", "test the journal: write, then power off mid-update",
              cmd_jcrash);

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: hexdump FILE [BYTES]\n");
        return 1;
    }
    u64 max = argc > 2 ? strtoull(argv[2], NULL, 0) : 256;
    int fd = fs_open(argv[1], O_READ);
    if (fd < 0)
        return fail("hexdump", argv[1], fd);
    u8 row[16];
    u64 off = 0;
    ssize_t n;
    while (off < max && (n = fs_pread(fd, off, row, MIN((u64)16, max - off))) > 0) {
        char line[96];
        int k = snprintf(line, sizeof(line), "%08llx ", (unsigned long long)off);
        for (int i = 0; i < 16; i++)
            k += snprintf(line + k, sizeof(line) - k, i < n ? " %02x" : "   ", row[i]);
        k += snprintf(line + k, sizeof(line) - k, "  ");
        for (int i = 0; i < n; i++)
            line[k++] = row[i] >= 32 && row[i] < 127 ? (char)row[i] : '.';
        line[k] = 0;
        printk("%s\n", line);
        off += (u64)n;
    }
    fs_close(fd);
    return 0;
}
SHELL_COMMAND(hexdump, "hexdump", "hexdump FILE [BYTES]", "show file bytes in hex", cmd_hexdump);

static int cmd_wc(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *data;
        size_t len;
        int r = fs_read_all(argv[i], &data, &len);
        if (r < 0) {
            rc = fail("wc", argv[i], r);
            continue;
        }
        u64 lines = 0, words = 0;
        bool inw = false;
        for (size_t k = 0; k < len; k++) {
            lines += data[k] == '\n';
            bool sp = isspace((u8)data[k]);
            words += !sp && !inw;
            inw = !sp;
        }
        printk("%7llu %7llu %9llu %s\n", (unsigned long long)lines, (unsigned long long)words,
               (unsigned long long)len, argv[i]);
        kfree(data);
    }
    return rc;
}
SHELL_COMMAND(wc, "wc", "wc FILE...", "count lines, words and bytes", cmd_wc);

static bool contains_ci(const char *hay, size_t hl, const char *needle, size_t nl)
{
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && tolower((u8)hay[i + k]) == tolower((u8)needle[k]))
            k++;
        if (k == nl)
            return true;
    }
    return false;
}

static int cmd_grep(int argc, char **argv)
{
    if (argc < 3) {
        printk("usage: grep TEXT FILE...   (case-insensitive)\n");
        return 1;
    }
    size_t nl = strlen(argv[1]);
    int hits = 0;
    for (int i = 2; i < argc; i++) {
        char *data;
        size_t len;
        int r = fs_read_all(argv[i], &data, &len);
        if (r < 0) {
            fail("grep", argv[i], r);
            continue;
        }
        size_t s = 0;
        for (int ln = 1; s < len; ln++) {
            size_t e = s;
            while (e < len && data[e] != '\n')
                e++;
            if (contains_ci(data + s, e - s, argv[1], nl)) {
                hits++;
                if (argc > 3)
                    printk("%s:", argv[i]);
                printk("%d: ", ln);
                console_write(data + s, e - s);
                printk("\n");
            }
            s = e + 1;
        }
        kfree(data);
    }
    return hits ? 0 : 1;
}
SHELL_COMMAND(grep, "grep", "grep TEXT FILE...", "find lines containing TEXT", cmd_grep);

static void find_walk(const char *dir, const char *pat, int *hits, int depth)
{
    struct fs_dirent *ents;
    int n;
    if (depth > 16 || fs_list(dir, &ents, &n) < 0)
        return;
    for (int i = 0; i < n; i++) {
        char *p = kmalloc(FS_PATH_MAX + FS_NAME_MAX + 2);
        snprintf(p, FS_PATH_MAX + FS_NAME_MAX + 2, "%s/%s", strcmp(dir, "/") ? dir : "",
                 ents[i].name);
        if (!*pat || contains_ci(ents[i].name, strlen(ents[i].name), pat, strlen(pat))) {
            printk("%s%s\n", p, ents[i].type == FT_DIR ? "/" : "");
            (*hits)++;
        }
        if (ents[i].type == FT_DIR)
            find_walk(p, pat, hits, depth + 1);
        kfree(p);
    }
    kfree(ents);
}

static int cmd_find(int argc, char **argv)
{
    char abs[FS_PATH_MAX];
    int r = fs_abspath(argc > 1 ? argv[1] : ".", abs, sizeof(abs));
    if (r < 0)
        return fail("find", argv[1], r);
    int hits = 0;
    find_walk(abs, argc > 2 ? argv[2] : "", &hits, 0);
    return hits ? 0 : 1;
}
SHELL_COMMAND(find, "find", "find [DIR] [NAME]", "list files under DIR whose name contains NAME",
              cmd_find);

/* ---- self-test: random operations checked against an in-memory shadow ---- */
#define FT_FILES 12
#define FT_MAX (192u << 10)

static u64 ft_rng;
static u32 ft_rand(void)
{
    ft_rng ^= ft_rng << 13;
    ft_rng ^= ft_rng >> 7;
    ft_rng ^= ft_rng << 17;
    return (u32)ft_rng;
}

static int cmd_fstest(int argc, char **argv)
{
    int ops = argc > 1 ? (int)strtol(argv[1], NULL, 10) : 300;
    ft_rng = 0x9e3779b97f4a7c15ULL ^ time_ns();
    u8 *shadow[FT_FILES];
    u32 size[FT_FILES];
    bool exists[FT_FILES];
    char name[FT_FILES][40];
    u8 *buf = kmalloc(FT_MAX), *chk = kmalloc(FT_MAX);
    for (int i = 0; i < FT_FILES; i++) {
        shadow[i] = kmalloc(FT_MAX);
        size[i] = 0;
        exists[i] = false;
        snprintf(name[i], sizeof(name[i]), "/tmp/fstest/d%d/f%d", i % 3, i);
    }
    fs_mkdir("/tmp/fstest");
    for (int d = 0; d < 3; d++) {
        char p[32];
        snprintf(p, sizeof(p), "/tmp/fstest/d%d", d);
        fs_mkdir(p);
    }
    int fails = 0, max_ext = 0;
    for (int op = 0; op < ops && !fails; op++) {
        int i = (int)(ft_rand() % FT_FILES), kind = (int)(ft_rand() % 10);
        if (kind < 4) { /* append a random chunk (interleaving fragments files) */
            u32 n = ft_rand() % 9000 + 1;
            if (size[i] + n > FT_MAX)
                n = FT_MAX - size[i];
            for (u32 k = 0; k < n; k++)
                buf[k] = (u8)ft_rand();
            int r = fs_append(name[i], buf, n);
            if (r < 0) {
                printk("fstest: append %s: %s\n", name[i], fs_strerror(r));
                fails++;
                break;
            }
            memcpy(shadow[i] + size[i], buf, n);
            size[i] += n;
            exists[i] = true;
        } else if (kind < 6) { /* overwrite whole file */
            u32 n = ft_rand() % (FT_MAX / 2);
            for (u32 k = 0; k < n; k++)
                buf[k] = (u8)ft_rand();
            int r = fs_write_all(name[i], buf, n);
            if (r < 0) {
                printk("fstest: write %s: %s\n", name[i], fs_strerror(r));
                fails++;
                break;
            }
            memcpy(shadow[i], buf, n);
            size[i] = n;
            exists[i] = true;
        } else if (kind < 7) { /* delete */
            int r = fs_unlink(name[i]);
            if (exists[i] ? r != FS_OK : r != FS_ENOENT) {
                printk("fstest: unlink %s: %s\n", name[i], fs_strerror(r));
                fails++;
            }
            exists[i] = false;
            size[i] = 0;
        } else if (kind < 8 && exists[i]) { /* rename into another directory */
            char to[40];
            int j = (int)(ft_rand() % FT_FILES);
            if (j != i && !exists[j]) {
                snprintf(to, sizeof(to), "%s", name[j]);
                int r = fs_rename(name[i], to);
                if (r < 0) {
                    printk("fstest: rename %s -> %s: %s\n", name[i], to, fs_strerror(r));
                    fails++;
                } else {
                    u8 *t = shadow[j];
                    shadow[j] = shadow[i];
                    shadow[i] = t;
                    size[j] = size[i];
                    exists[j] = true;
                    size[i] = 0;
                    exists[i] = false;
                }
            }
        }
        /* verify one random file, and every file now and then */
        for (int v = 0; v < FT_FILES && !fails; v++) {
            if (v != i && op % 50)
                continue;
            struct fs_stat st;
            int r = fs_stat(name[v], &st);
            if (!exists[v]) {
                if (r != FS_ENOENT) {
                    printk("fstest: %s should not exist\n", name[v]);
                    fails++;
                }
                continue;
            }
            if (r < 0 || st.size != size[v]) {
                printk("fstest: %s: size %llu, expected %u (%s)\n", name[v],
                       (unsigned long long)st.size, size[v], fs_strerror(r));
                fails++;
                continue;
            }
            if ((int)st.nextents > max_ext)
                max_ext = (int)st.nextents;
            int fd = fs_open(name[v], O_READ);
            ssize_t got = fs_pread(fd, 0, chk, size[v]);
            fs_close(fd);
            if (got != (ssize_t)size[v] || memcmp(chk, shadow[v], size[v])) {
                printk("fstest: %s: content mismatch\n", name[v]);
                fails++;
            }
        }
    }
    for (int i = 0; i < FT_FILES; i++)
        kfree(shadow[i]);
    kfree(buf);
    kfree(chk);
    printk("fstest: %d operations, %s (most extents in one file: %d)\n", ops,
           fails ? "FAILED" : "all verified", max_ext);
    return fails ? 1 : 0;
}
SHELL_COMMAND(fstest, "fstest", "fstest [OPS]", "file system self-test (uses /tmp/fstest)",
              cmd_fstest);
