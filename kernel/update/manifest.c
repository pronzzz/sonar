/* Update manifests and /boot/kernel bundles (format in update.h). Shared by
 * the kernel and the UEFI loader stub: no allocation, no output, and no
 * initialised pointers in data (the stub runs before relocation). */
#include "crypto.h"
#include "string.h"
#include "update.h"

/* make UPDATE_KEY=other.h builds with another key (the updater's tests) */
#ifdef UPDATE_KEY_FILE
#include UPDATE_KEY_FILE
#else
#include "update_key.h"
#endif

const u8 *update_public_key(void) { return update_pubkey; }

const char *manifest_strerror(int e)
{
    switch (e) {
    case MF_OK: return "verified";
    case MF_FORMAT: return "not a Sonar update manifest";
    case MF_FIELDS: return "the manifest is missing a field";
    case MF_SIGNATURE: return "BAD SIGNATURE: not signed with Sonar's update key";
    case MF_ARCH: return "the update is not for aarch64";
    case MF_SIZE: return "the kernel image is not the size the manifest says";
    case MF_HASH: return "the kernel image does not match the manifest's SHA-256";
    case MF_IMAGE: return "the file is not a Sonar kernel image";
    case MF_BUNDLE: return "not an installed update (bad /boot/kernel header)";
    }
    return "invalid update";
}

static bool starts(const char *line, size_t n, const char *key, const char **val, size_t *vlen)
{
    size_t k = strlen(key);
    if (n <= k || memcmp(line, key, k) || line[k] != ' ')
        return false;
    *val = line + k + 1;
    *vlen = n - k - 1;
    return true;
}

static bool copy_field(char *dst, size_t cap, const char *v, size_t n)
{
    if (!n || n >= cap)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((u8)v[i] < 0x21 || (u8)v[i] > 0x7e) /* printable, no spaces */
            return false;
    memcpy(dst, v, n);
    dst[n] = 0;
    return true;
}

int manifest_verify(const char *text, size_t len, struct manifest *m)
{
    static const char magic[] = "sonar-update 1\n";
    memset(m, 0, sizeof(*m));
    if (len > MANIFEST_MAX || len < sizeof(magic) - 1 || memcmp(text, magic, sizeof(magic) - 1))
        return MF_FORMAT;
    /* the signature line is the last line */
    size_t end = len;
    while (end && (text[end - 1] == '\n' || text[end - 1] == '\r'))
        end--;
    size_t start = end;
    while (start && text[start - 1] != '\n')
        start--;
    const char *sigv;
    size_t sigl;
    u8 sig[64];
    if (!starts(text + start, end - start, "ed25519", &sigv, &sigl) ||
        !hex_decode(sigv, sigl, sig, sizeof(sig)))
        return MF_FORMAT;
    if (!ed25519_verify(sig, text, start, update_public_key()))
        return MF_SIGNATURE;

    /* Only now, with the text known to be ours, parse it. */
    bool have_version = false, have_file = false, have_size = false, have_sha = false;
    for (size_t pos = sizeof(magic) - 1; pos < start;) {
        size_t e = pos;
        while (e < start && text[e] != '\n')
            e++;
        const char *line = text + pos, *v;
        size_t n = e - pos, vl;
        if (n && line[n - 1] == '\r')
            n--;
        if (starts(line, n, "version", &v, &vl))
            have_version = copy_field(m->version, sizeof(m->version), v, vl);
        else if (starts(line, n, "arch", &v, &vl))
            copy_field(m->arch, sizeof(m->arch), v, vl);
        else if (starts(line, n, "file", &v, &vl))
            have_file = copy_field(m->file, sizeof(m->file), v, vl);
        else if (starts(line, n, "sha256", &v, &vl))
            have_sha = hex_decode(v, vl, m->sha256, 32);
        else if (starts(line, n, "key", &v, &vl))
            copy_field(m->key, sizeof(m->key), v, vl);
        else if (starts(line, n, "size", &v, &vl)) {
            u64 s = 0;
            size_t i = 0;
            for (; i < vl && v[i] >= '0' && v[i] <= '9' && i < 12; i++)
                s = s * 10 + (u64)(v[i] - '0');
            have_size = i == vl && s > 0;
            m->size = s;
        }
        pos = e + 1;
    }
    if (!have_version || !have_file || !have_size || !have_sha)
        return MF_FIELDS;
    for (const char *p = m->version; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.'))
            return MF_FIELDS;
    if (m->arch[0] && strcmp(m->arch, "aarch64"))
        return MF_ARCH;
    return MF_OK;
}

int manifest_check_image(const struct manifest *m, const void *image, size_t len)
{
    if (len != m->size)
        return MF_SIZE;
    u8 h[32];
    sha256(image, len, h);
    if (!crypto_equal(h, m->sha256, 32))
        return MF_HASH;
    /* Sonar's Image is both a PE/COFF EFI application ("MZ") and an arm64
     * Linux Image ("ARM\x64" at offset 56). Signed images always are; this
     * catches a release built wrongly before anything boots it. */
    const u8 *p = image;
    if (len < 4096 || p[0] != 'M' || p[1] != 'Z' || memcmp(p + 56, "ARM\x64", 4))
        return MF_IMAGE;
    return MF_OK;
}

int bundle_verify(const void *bundle, size_t len, struct manifest *m, const void **image,
                  size_t *image_len)
{
    const u8 *b = bundle;
    if (len < BUNDLE_HEADER || memcmp(b, BUNDLE_MAGIC, 8))
        return MF_BUNDLE;
    u32 ml = (u32)b[8] | (u32)b[9] << 8 | (u32)b[10] << 16 | (u32)b[11] << 24;
    if (ml > BUNDLE_HEADER - 16 || ml > MANIFEST_MAX)
        return MF_BUNDLE;
    int r = manifest_verify((const char *)b + 16, ml, m);
    if (r != MF_OK)
        return r;
    *image = b + BUNDLE_HEADER;
    *image_len = len - BUNDLE_HEADER;
    return manifest_check_image(m, *image, *image_len);
}

int version_compare(const char *a, const char *b)
{
    while (*a || *b) {
        u64 x = 0, y = 0;
        while (*a >= '0' && *a <= '9')
            x = x * 10 + (u64)(*a++ - '0');
        while (*b >= '0' && *b <= '9')
            y = y * 10 + (u64)(*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        else if (*a)
            return 0; /* not a number: treat the rest as equal */
        if (*b == '.')
            b++;
        else if (*b)
            return 0;
    }
    return 0;
}
