/*
 * The `update` command: signed kernel updates from GitHub Releases
 * (docs/updates.md).
 *
 *   update check      is there a newer signed release?
 *   update install    download, verify, and install it to /boot/kernel
 *   update status     what is running and what is installed
 *   update rollback   swap /boot/kernel and /boot/kernel.prev
 *   update clear      set /boot/kernel aside: the built-in kernel boots
 *   update verify MANIFEST [IMAGE]   check files on disk
 *
 * Settings (`config KEY VALUE`):
 *   update.repo    GitHub repository, default pronzzz/sonar
 *   update.token   GitHub token, only needed while the repository is private
 *   update.api     GitHub API base URL, default https://api.github.com
 *   update.mirror  base URL of a stand-in server holding sonar-update.txt and
 *                  the image (instead of GitHub)
 *   update.auto    off (default), check: say so at boot when an update is
 *                  available, install: also install it
 *
 * Everything downloaded is checked against the Ed25519 key compiled into
 * this kernel before it is used, so the HTTPS connection need not (and,
 * with kernel/net/tls.c, does not) authenticate GitHub.
 */
#include "arch.h"
#include "config.h"
#include "crypto.h"
#include "efi.h"
#include "fs.h"
#include "http.h"
#include "irq.h"
#include "../ai/json.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "shell.h"
#include "string.h"
#include "update.h"

#define DEFAULT_REPO "pronzzz/sonar"
#define DEFAULT_API "https://api.github.com"
#define MANIFEST_NAME "sonar-update.txt"
#define MAX_IMAGE (64ULL << 20)

/* ------------------------------------------------------------ downloads */
struct buf {
    u8 *p;
    size_t len, cap;
    bool pages;    /* p came from page_alloc (cap is a page multiple) */
    bool progress; /* show a progress line */
    u64 last_ms;
};

static bool buf_put(void *ctx, const void *d, size_t n)
{
    struct buf *b = ctx;
    if (b->len + n > b->cap)
        return false;
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return true;
}

static void buf_free(struct buf *b)
{
    if (b->pages)
        page_free(b->p, b->cap / PAGE_SIZE);
    else
        kfree(b->p);
    b->p = NULL;
}

static void show_progress(void *ctx, u64 done, u64 total)
{
    struct buf *b = ctx;
    u64 now = time_ms();
    if (!b->progress || (now - b->last_ms < 200 && done != total))
        return;
    b->last_ms = now;
    if (total)
        printk("\r  %llu / %llu KiB  %3llu%%", (unsigned long long)(done / 1024),
               (unsigned long long)(total / 1024), (unsigned long long)(done * 100 / total));
    if (total && done == total)
        printk("\n");
}

struct source {
    bool github;
    char repo[96];
    char api[128];
    char mirror[256];
    const char *token;
    /* the release */
    char tag[48];
    int nassets;
    struct {
        char name[96];
        char url[256];
    } assets[16];
};

/* A setting, or "" when unset ("none" and "-" also mean unset). */
static const char *setting(const char *key, const char *def)
{
    const char *v = config_get(key, NULL);
    if (!v || !*v)
        return def;
    if (!strcmp(v, "none") || !strcmp(v, "-") || !strcmp(v, "\"\""))
        return "";
    return v;
}

static void load_source(struct source *s)
{
    memset(s, 0, sizeof(*s));
    const char *mirror = setting("update.mirror", "");
    if (*mirror) {
        strlcpy(s->mirror, mirror, sizeof(s->mirror));
        size_t n = strlen(s->mirror);
        while (n && s->mirror[n - 1] == '/')
            s->mirror[--n] = 0;
        return;
    }
    s->github = true;
    strlcpy(s->repo, *setting("update.repo", "") ? setting("update.repo", "") : DEFAULT_REPO,
            sizeof(s->repo));
    strlcpy(s->api, *setting("update.api", "") ? setting("update.api", "") : DEFAULT_API,
            sizeof(s->api));
    const char *t = setting("update.token", "");
    s->token = *t ? t : NULL;
}

static void describe_source(const struct source *s, char *out, size_t n)
{
    if (s->github)
        snprintf(out, n, "github.com/%s%s", s->repo, s->token ? " (with a token)" : "");
    else
        snprintf(out, n, "%s", s->mirror);
}

/* GET url into a buffer of at most cap bytes. */
static bool get(const struct source *s, const char *url, const char *accept, struct buf *b,
                size_t cap, bool progress, char *err, size_t errlen)
{
    struct http_get *g = kzalloc(sizeof(*g));
    char *auth = NULL;
    if (!g)
        return false;
    b->len = 0;
    b->cap = cap;
    b->pages = cap > 64 * 1024;
    if (b->pages) {
        b->cap = ALIGN_UP(cap, PAGE_SIZE);
        b->p = page_alloc_nozero(b->cap / PAGE_SIZE);
    } else {
        b->p = kmalloc(cap + 1);
    }
    if (!b->p) {
        snprintf(err, errlen, "out of memory for %llu bytes", (unsigned long long)cap);
        kfree(g);
        return false;
    }
    if (s->token && s->github) {
        auth = kmalloc(160);
        if (auth)
            snprintf(auth, 160, "Bearer %s", s->token);
    }
    g->url = url;
    g->accept = accept;
    g->auth = auth;
    g->max_bytes = cap;
    g->timeout_ms = 20000;
    g->body = buf_put;
    g->ctx = b;
    b->progress = progress;
    b->last_ms = 0;
    g->progress = show_progress;
    bool ok = http_get(g);
    if (!ok) {
        if (g->status == 404 && s->github)
            snprintf(err, errlen,
                     "nothing at %s (HTTP 404).\n"
                     "  If github.com/%s is private, devices cannot see its releases: make the\n"
                     "  repository public, or give this computer a read-only token with\n"
                     "  'config update.token TOKEN' (see docs/updates.md).",
                     url, s->repo);
        else if ((g->status == 401 || g->status == 403) && s->github)
            snprintf(err, errlen, "%s%s", g->error,
                     g->status == 401 ? " (update.token is not accepted)"
                                      : " (GitHub refused: rate limit or token permissions)");
        else
            snprintf(err, errlen, "%s", g->error);
        buf_free(b);
    } else if (!b->pages) {
        b->p[b->len] = 0;
    }
    if (auth) {
        crypto_wipe(auth, 160);
        kfree(auth);
    }
    kfree(g);
    return ok;
}

/* Find the latest release (GitHub) and its assets. */
static bool find_release(struct source *s, char *err, size_t errlen)
{
    if (!s->github)
        return true;
    char url[256];
    snprintf(url, sizeof(url), "%s/repos/%s/releases/latest", s->api, s->repo);
    struct buf b;
    if (!get(s, url, "application/vnd.github+json", &b, 256 * 1024, false, err, errlen))
        return false;
    struct jval *j = json_parse((const char *)b.p, b.len);
    buf_free(&b);
    if (!j || j->type != J_OBJ) {
        snprintf(err, errlen, "GitHub's answer for %s is not JSON", url);
        json_release(j);
        return false;
    }
    const char *tag = json_str(json_get(j, "tag_name"));
    strlcpy(s->tag, tag ? tag : "?", sizeof(s->tag));
    const struct jval *assets = json_get(j, "assets");
    s->nassets = 0;
    if (assets && assets->type == J_ARR)
        for (int i = 0; i < assets->n && s->nassets < (int)ARRAY_SIZE(s->assets); i++) {
            const char *name = json_str(json_get(&assets->items[i], "name"));
            const char *aurl = json_str(json_get(&assets->items[i], "url"));
            if (!name || !aurl)
                continue;
            strlcpy(s->assets[s->nassets].name, name, sizeof(s->assets[0].name));
            strlcpy(s->assets[s->nassets].url, aurl, sizeof(s->assets[0].url));
            s->nassets++;
        }
    json_release(j);
    return true;
}

/* URL of a release file, and the Accept header to use for it. */
static bool asset_url(const struct source *s, const char *name, char *url, size_t n,
                      const char **accept)
{
    if (!s->github) {
        snprintf(url, n, "%s/%s", s->mirror, name);
        *accept = NULL;
        return true;
    }
    for (int i = 0; i < s->nassets; i++)
        if (!strcmp(s->assets[i].name, name)) {
            /* the API asset URL works for public and (with a token) private
             * repositories; GitHub redirects it to its download servers */
            strlcpy(url, s->assets[i].url, n);
            *accept = "application/octet-stream";
            return true;
        }
    return false;
}

struct found {
    struct source src;
    char *text; /* the manifest */
    size_t len;
    struct manifest m;
};

/* Fetch and verify the latest manifest. Prints what it finds. */
static bool fetch_manifest(struct found *f, bool quiet)
{
    char err[400], where[200], url[256];
    const char *accept;
    memset(f, 0, sizeof(*f));
    load_source(&f->src);
    describe_source(&f->src, where, sizeof(where));
    if (!net_iface() || !net_iface()->ip) {
        printk("update: no network connection (run 'dhcp')\n");
        return false;
    }
    if (!quiet)
        printk("update: checking %s\n", where);
    if (!find_release(&f->src, err, sizeof(err))) {
        printk("update: %s\n", err);
        return false;
    }
    if (!asset_url(&f->src, MANIFEST_NAME, url, sizeof(url), &accept)) {
        printk("update: release %s of %s has no %s (it was not made by tools/release.sh)\n",
               f->src.tag, where, MANIFEST_NAME);
        return false;
    }
    struct buf b;
    if (!get(&f->src, url, accept, &b, MANIFEST_MAX, false, err, sizeof(err))) {
        printk("update: %s\n", err);
        return false;
    }
    f->text = (char *)b.p;
    f->len = b.len;
    int r = manifest_verify(f->text, f->len, &f->m);
    if (r != MF_OK) {
        printk("update: REJECTED %s from %s: %s\n", MANIFEST_NAME, where, manifest_strerror(r));
        kfree(f->text);
        f->text = NULL;
        return false;
    }
    return true;
}

static void found_free(struct found *f)
{
    kfree(f->text);
    f->text = NULL;
}

/* ------------------------------------------------------------- install */
static bool booted_from_disk(void) { return efi_booted(); }

static bool write_all(int fd, const void *p, size_t n)
{
    return fs_write(fd, p, n) == (ssize_t)n;
}

static bool install(const struct manifest *m, const char *text, size_t tlen, const u8 *img)
{
    if (fs_is_ram()) {
        printk("update: the system disk is held in memory (sonar/system.img on the EFI\n"
               "  partition), so an installed kernel would be lost at power-off. Updates need a\n"
               "  disk with a SonarFS system partition (tools/mkesp.py --system).\n");
        return false;
    }
    fs_mkdir("/boot");
    int fd = fs_open(UPDATE_NEW_PATH, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        printk("update: %s: %s\n", UPDATE_NEW_PATH, fs_strerror(fd));
        return false;
    }
    u8 *head = kzalloc(BUNDLE_HEADER);
    bool ok = head != NULL;
    if (ok) {
        memcpy(head, BUNDLE_MAGIC, 8);
        head[8] = (u8)tlen;
        head[9] = (u8)(tlen >> 8);
        memcpy(head + 16, text, tlen);
        ok = write_all(fd, head, BUNDLE_HEADER) && write_all(fd, img, m->size);
    }
    kfree(head);
    fs_close(fd);
    if (!ok) {
        printk("update: could not write %s (disk full?)\n", UPDATE_NEW_PATH);
        fs_unlink(UPDATE_NEW_PATH);
        return false;
    }
    /* Read it back and check it exactly as the loader will. */
    char *back;
    size_t blen;
    struct manifest bm;
    const void *bi;
    size_t bil;
    int r = fs_read_all(UPDATE_NEW_PATH, &back, &blen);
    if (r < 0 || (r = bundle_verify(back, blen, &bm, &bi, &bil)) != MF_OK) {
        printk("update: the copy on disk does not verify (%s); not installed\n",
               r < 0 ? fs_strerror(r) : manifest_strerror(r));
        if (r >= 0)
            kfree(back);
        fs_unlink(UPDATE_NEW_PATH);
        return false;
    }
    kfree(back);
    struct fs_stat st;
    if (fs_stat(UPDATE_PATH, &st) == FS_OK && (r = fs_rename(UPDATE_PATH, UPDATE_PREV_PATH)) < 0) {
        printk("update: could not keep the previous update: %s\n", fs_strerror(r));
        return false;
    }
    if ((r = fs_rename(UPDATE_NEW_PATH, UPDATE_PATH)) < 0) {
        printk("update: could not install %s: %s\n", UPDATE_PATH, fs_strerror(r));
        return false;
    }
    return true;
}

static void say_newer(const struct found *f)
{
    printk("update: Sonar %s is available (this is %s)%s%s\n", f->m.version, OS_VERSION,
           f->src.github ? ", release " : "", f->src.github ? f->src.tag : "");
}

static int do_install(bool force, bool quiet)
{
    struct found f;
    if (!fetch_manifest(&f, quiet))
        return 1;
    int cmp = version_compare(f.m.version, OS_VERSION);
    if (cmp <= 0 && !force) {
        printk("update: Sonar %s is up to date (latest signed release: %s)\n", OS_VERSION,
               f.m.version);
        found_free(&f);
        return 0;
    }
    say_newer(&f);
    if (f.m.size > MAX_IMAGE) {
        printk("update: the image is too large (%llu bytes)\n", (unsigned long long)f.m.size);
        found_free(&f);
        return 1;
    }
    char url[256], err[400];
    const char *accept;
    if (!asset_url(&f.src, f.m.file, url, sizeof(url), &accept)) {
        printk("update: the release has no file %s\n", f.m.file);
        found_free(&f);
        return 1;
    }
    printk("update: downloading %s (%llu KiB)\n", f.m.file,
           (unsigned long long)((f.m.size + 1023) / 1024));
    struct buf img;
    u64 t0 = time_ms();
    if (!get(&f.src, url, accept, &img, f.m.size, true, err, sizeof(err))) {
        printk("update: %s\n", err);
        found_free(&f);
        return 1;
    }
    int r = manifest_check_image(&f.m, img.p, img.len);
    if (r != MF_OK) {
        printk("update: REJECTED %s: %s\n", f.m.file, manifest_strerror(r));
        buf_free(&img);
        found_free(&f);
        return 1;
    }
    char sha[65];
    hex_encode(f.m.sha256, 32, sha);
    printk("update: signature and SHA-256 verified (%.16s...), %llu ms\n", sha,
           (unsigned long long)(time_ms() - t0));
    bool ok = install(&f.m, f.text, f.len, img.p);
    buf_free(&img);
    if (ok) {
        printk("update: installed Sonar %s as %s (the previous one is kept as %s)\n",
               f.m.version, UPDATE_PATH, UPDATE_PREV_PATH);
        if (!booted_from_disk())
            printk("update: this computer was not started through UEFI (QEMU -kernel or a\n"
                   "  bootloader that loads the kernel itself), so it keeps booting the kernel it\n"
                   "  is given; the update starts when Sonar boots from its disk.\n");
        else
            printk("update: restart to use it. The loader checks it again at every boot and\n"
                   "  falls back to the built-in kernel %s if it does not start.\n", OS_VERSION);
    }
    found_free(&f);
    return ok ? 0 : 1;
}

static int do_check(bool quiet)
{
    struct found f;
    if (!fetch_manifest(&f, quiet))
        return 1;
    char sha[65];
    hex_encode(f.m.sha256, 32, sha);
    if (version_compare(f.m.version, OS_VERSION) > 0) {
        say_newer(&f);
        if (!quiet)
            printk("  signed manifest verified: %s, %llu bytes, sha256 %s\n"
                   "  run 'update install' to install it\n",
                   f.m.file, (unsigned long long)f.m.size, sha);
    } else {
        printk("update: Sonar %s is up to date (latest signed release: %s)\n", OS_VERSION,
               f.m.version);
    }
    found_free(&f);
    return 0;
}

/* -------------------------------------------------------------- status */
static void show_bundle(const char *path)
{
    char *b;
    size_t n;
    int r = fs_read_all(path, &b, &n);
    if (r < 0) {
        printk("  %-18s none\n", path);
        return;
    }
    struct manifest m;
    const void *img;
    size_t il;
    r = bundle_verify(b, n, &m, &img, &il);
    if (r == MF_OK)
        printk("  %-18s Sonar %s, %llu KiB, verified\n", path, m.version,
               (unsigned long long)(m.size + 1023) / 1024);
    else
        printk("  %-18s %s\n", path, manifest_strerror(r));
    kfree(b);
}

static int do_status(void)
{
    struct source s;
    char where[200];
    load_source(&s);
    describe_source(&s, where, sizeof(where));
    printk("running:  Sonar %s, %s\n", OS_VERSION,
           efi_handoff.chained ? "installed update (/boot/kernel)"
           : efi_booted()      ? "built-in kernel (EFI partition)"
                               : "kernel loaded directly (not UEFI)");
    if (efi_handoff.update_note[0])
        printk("loader:   %s\n", efi_handoff.update_note);
    printk("installed:\n");
    show_bundle(UPDATE_PATH);
    show_bundle(UPDATE_PREV_PATH);
    printk("source:   %s\n", where);
    printk("auto:     update.auto = %s\n", config_get("update.auto", "off"));
    char key[65];
    hex_encode(update_public_key(), 32, key);
    printk("key:      ed25519 %s\n", key);
    return 0;
}

static int do_verify(const char *mpath, const char *ipath)
{
    char *text;
    size_t len;
    int r = fs_read_all(mpath, &text, &len);
    if (r < 0) {
        printk("update: %s: %s\n", mpath, fs_strerror(r));
        return 1;
    }
    struct manifest m;
    const void *img;
    size_t il;
    if (len >= 8 && !memcmp(text, BUNDLE_MAGIC, 8))
        r = bundle_verify(text, len, &m, &img, &il);
    else
        r = manifest_verify(text, len, &m);
    if (r == MF_OK && ipath) {
        char *data;
        size_t dl;
        int e = fs_read_all(ipath, &data, &dl);
        if (e < 0) {
            printk("update: %s: %s\n", ipath, fs_strerror(e));
            kfree(text);
            return 1;
        }
        r = manifest_check_image(&m, data, dl);
        kfree(data);
    }
    kfree(text);
    if (r != MF_OK) {
        printk("update: %s: REJECTED: %s\n", mpath, manifest_strerror(r));
        return 1;
    }
    printk("update: %s: verified (Sonar %s, %s, %llu bytes)\n", mpath, m.version, m.file,
           (unsigned long long)m.size);
    return 0;
}

static int do_rollback(void)
{
    struct fs_stat st;
    if (fs_stat(UPDATE_PREV_PATH, &st) != FS_OK) {
        printk("update: there is no %s to go back to\n", UPDATE_PREV_PATH);
        return 1;
    }
    bool have_cur = fs_stat(UPDATE_PATH, &st) == FS_OK;
    int r = FS_OK;
    if (have_cur)
        r = fs_rename(UPDATE_PATH, UPDATE_NEW_PATH);
    if (r == FS_OK)
        r = fs_rename(UPDATE_PREV_PATH, UPDATE_PATH);
    if (r == FS_OK && have_cur)
        r = fs_rename(UPDATE_NEW_PATH, UPDATE_PREV_PATH);
    if (r != FS_OK) {
        printk("update: rollback failed: %s\n", fs_strerror(r));
        return 1;
    }
    printk("update: swapped %s and %s; the loader starts %s only if it is newer than the\n"
           "  built-in kernel\n",
           UPDATE_PATH, UPDATE_PREV_PATH, UPDATE_PATH);
    show_bundle(UPDATE_PATH);
    return 0;
}

static int do_clear(void)
{
    struct fs_stat st;
    if (fs_stat(UPDATE_PATH, &st) != FS_OK) {
        printk("update: no update is installed; the built-in kernel starts\n");
        return 0;
    }
    int r = fs_rename(UPDATE_PATH, UPDATE_PREV_PATH);
    if (r != FS_OK) {
        printk("update: %s\n", fs_strerror(r));
        return 1;
    }
    printk("update: moved %s to %s; the built-in kernel starts next time\n", UPDATE_PATH,
           UPDATE_PREV_PATH);
    return 0;
}

static int cmd_update(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "check";
    bool force = argc > 2 && !strcmp(argv[2], "--force");
    if (!strcmp(sub, "check"))
        return do_check(false);
    if (!strcmp(sub, "install"))
        return do_install(force, false);
    if (!strcmp(sub, "status"))
        return do_status();
    if (!strcmp(sub, "verify") && argc >= 3)
        return do_verify(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcmp(sub, "rollback"))
        return do_rollback();
    if (!strcmp(sub, "clear"))
        return do_clear();
    printk("usage: update [check | install [--force] | status | rollback | clear |\n"
           "               verify MANIFEST [IMAGE]]\n"
           "  Signed kernel updates from GitHub releases (docs/updates.md). Settings:\n"
           "  update.repo, update.token, update.mirror, update.auto (off, check, install).\n");
    return 1;
}
SHELL_COMMAND(update, "update", "update [check|install|status|rollback|clear]",
              "signed system updates from GitHub", cmd_update);

static int cmd_sha256(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: sha256 FILE...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = fs_open(argv[i], O_READ);
        if (fd < 0) {
            printk("sha256: %s: %s\n", argv[i], fs_strerror(fd));
            rc = 1;
            continue;
        }
        static u8 chunk[16384];
        struct sha256 c;
        sha256_init(&c);
        ssize_t n;
        while ((n = fs_read(fd, chunk, sizeof(chunk))) > 0)
            sha256_update(&c, chunk, (size_t)n);
        fs_close(fd);
        u8 h[32];
        char hx[65];
        sha256_final(&c, h);
        hex_encode(h, 32, hx);
        printk("%s  %s\n", hx, argv[i]);
    }
    return rc;
}
SHELL_COMMAND(sha256, "sha256", "sha256 FILE...", "print files' SHA-256 checksums", cmd_sha256);

/* --------------------------------------------------------------- boot */
void update_boot(void)
{
#ifdef SONAR_TEST_FAIL_BOOT
    /* tools/test_update.py: a kernel that never finishes starting */
    printk("update: test build: resetting before the start is confirmed\n");
    psci_system_reset();
#endif
    if (efi_handoff.update_note[0])
        printk("update: %s\n", efi_handoff.update_note);
    if (efi_handoff.chained && efi_handoff.chained_sha[0]) {
        /* Reaching this point (file system and network up) counts as a good
         * start: tell the loader to stop counting attempts. */
        char *cur;
        size_t n;
        bool known = false;
        if (fs_read_all(UPDATE_OK_PATH, &cur, &n) >= 0) {
            known = n >= 64 && !memcmp(cur, efi_handoff.chained_sha, 64);
            kfree(cur);
        }
        if (!known) {
            char line[66];
            snprintf(line, sizeof(line), "%s\n", efi_handoff.chained_sha);
            int r = fs_write_all(UPDATE_OK_PATH, line, 65);
            printk("update: running installed update Sonar %s%s\n", OS_VERSION,
                   r < 0 ? " (could not mark it as good)" : "; marked as good");
        }
    }
    const char *mode = config_get("update.auto", "off");
    bool check = !strcmp(mode, "check") || !strcmp(mode, "on") || !strcmp(mode, "yes") ||
                 !strcmp(mode, "true") || !strcmp(mode, "1");
    bool inst = !strcmp(mode, "install");
    if (!check && !inst)
        return;
    if (!net_iface() || !net_iface()->ip) {
        printk("update: not checking for updates: no network\n");
        return;
    }
    if (inst)
        do_install(false, true);
    else
        do_check(true);
}
