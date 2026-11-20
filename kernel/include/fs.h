/*
 * File system interface. Sonar has one root file system, SonarFS (see
 * docs/sonarfs.md), mounted from the system disk or, when there is none,
 * from a RAM disk (contents are then lost at power-off).
 */
#pragma once
#include "block.h"
#include "types.h"

enum { FT_NONE = 0, FT_FILE = 1, FT_DIR = 2 };

#define FS_NAME_MAX 58
#define FS_PATH_MAX 256

enum {
    FS_OK = 0,
    FS_ENOENT = -2,
    FS_EEXIST = -3,
    FS_ENOTDIR = -4,
    FS_EISDIR = -5,
    FS_ENOSPC = -6,
    FS_EIO = -7,
    FS_EINVAL = -8,
    FS_ENOTEMPTY = -9,
    FS_EROFS = -10,
    FS_ENAMETOOLONG = -11,
    FS_EBADF = -12,
    FS_EMFILE = -13,
    FS_ENOMEM = -14,
    FS_ENOFS = -15,
    FS_EBUSY = -16,
};
const char *fs_strerror(int e);

struct fs_stat {
    u32 ino;
    int type;
    u64 size;
    u64 ctime, mtime; /* seconds since the Unix epoch */
    u16 links;
    u32 nextents;
};

struct fs_dirent {
    u32 ino;
    int type;
    char name[FS_NAME_MAX + 1];
};

/* Format / mount */
bool fs_mkfs(struct blkdev *d, const char *label, char *err, size_t errlen);
bool fs_probe(struct blkdev *d); /* has a SonarFS superblock? */
int fs_mount(struct blkdev *d, bool ram);
bool fs_mounted(void);
bool fs_is_ram(void);
const char *fs_device(void);
void fs_usage(u64 *total_bytes, u64 *free_bytes, char *label, size_t labellen);
void fs_boot(void); /* find and mount the root file system */

/* Paths (relative paths resolve against the current directory) */
const char *fs_cwd(void);
int fs_chdir(const char *path);
int fs_abspath(const char *path, char *out, size_t n);

/* Namespace */
int fs_stat(const char *path, struct fs_stat *st);
int fs_mkdir(const char *path);
int fs_unlink(const char *path);
int fs_rmdir(const char *path);
int fs_rename(const char *from, const char *to);
/* Directory listing: *out is a kmalloc'd array of *n entries (without . and ..). */
int fs_list(const char *path, struct fs_dirent **out, int *n);

/* Files */
enum { O_READ = 1, O_WRITE = 2, O_CREATE = 4, O_TRUNC = 8, O_APPEND = 16 };
int fs_open(const char *path, int flags);
ssize_t fs_read(int fd, void *buf, size_t n);
ssize_t fs_write(int fd, const void *buf, size_t n);
ssize_t fs_pread(int fd, u64 off, void *buf, u64 n);
int fs_seek(int fd, u64 off);
u64 fs_fsize(int fd);
int fs_close(int fd);

/* Whole-file helpers. fs_read_all returns a NUL-terminated kmalloc'd buffer. */
int fs_read_all(const char *path, char **out, size_t *len);
int fs_write_all(const char *path, const void *data, size_t len);
int fs_append(const char *path, const void *data, size_t len);
