/*
 * Signed updates (docs/updates.md).
 *
 * A release has two files: the kernel image and a manifest,
 *
 *     sonar-update 1
 *     version 0.2.0
 *     arch aarch64
 *     file sonar-0.2.0-Image
 *     size 475136
 *     sha256 <64 hex digits>
 *     key <first 16 hex digits of the public key>
 *     ed25519 <128 hex digits>
 *
 * where the last line is an Ed25519 signature of every byte before it,
 * checked against the public key compiled into the kernel
 * (kernel/update/update_key.h). `update install` stores both on the system
 * disk as one file, /boot/kernel: "SONARUPD", u32 manifest length, u32 0,
 * the manifest, zeros up to 4096 bytes, then the image. The UEFI loader
 * checks it again at every boot before starting it.
 *
 * manifest.c is shared with the loader stub (efi_update.c), so it uses no
 * memory allocation, no output and no initialised pointers in data.
 */
#pragma once
#include "types.h"

#define MANIFEST_MAX 2048
#define BUNDLE_MAGIC "SONARUPD"
#define BUNDLE_HEADER 4096
#define UPDATE_PATH "/boot/kernel"
#define UPDATE_PREV_PATH "/boot/kernel.prev"
#define UPDATE_NEW_PATH "/boot/kernel.new"
#define UPDATE_OK_PATH "/boot/ok"
#define UPDATE_MAX_TRIES 3

struct manifest {
    char version[24];
    char arch[16];
    char file[96];
    u64 size;
    u8 sha256[32];
    char key[17];
};

enum {
    MF_OK = 0,
    MF_FORMAT,      /* not a Sonar update manifest */
    MF_FIELDS,      /* a required field is missing or malformed */
    MF_SIGNATURE,   /* the signature does not match the built-in key */
    MF_ARCH,        /* not for aarch64 */
    MF_SIZE,        /* the image is not the size the manifest says */
    MF_HASH,        /* the image's SHA-256 does not match */
    MF_IMAGE,       /* not a Sonar kernel image (PE/arm64 Image headers) */
    MF_BUNDLE,      /* /boot/kernel is not a bundle */
};
const char *manifest_strerror(int e);

/* The Ed25519 public key updates must be signed with. */
const u8 *update_public_key(void);

/* Check the signature and parse the fields. */
int manifest_verify(const char *text, size_t len, struct manifest *m);
/* Check an image against a verified manifest (size, SHA-256, headers). */
int manifest_check_image(const struct manifest *m, const void *image, size_t len);
/* Check a whole /boot/kernel bundle; *image and *image_len point into it. */
int bundle_verify(const void *bundle, size_t len, struct manifest *m, const void **image,
                  size_t *image_len);

/* Compare dotted version numbers ("0.10.1" > "0.9.3"); <0, 0, >0. */
int version_compare(const char *a, const char *b);

/* Kernel side (kernel/update/update.c): at boot, after the network is up. */
void update_boot(void);
