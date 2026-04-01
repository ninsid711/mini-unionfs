/*
 * mini_unionfs.h
 *
 * Shared header for Mini-UnionFS (Team 6)
 * Written by: Member 1
 *
 * Every member includes this file. It exposes:
 *   - The global state struct
 *   - The UNIONFS_DATA context macro
 *   - resolve_path() and path-building helpers
 *
 * Members 2-4 only need to call:
 *   resolve_path(path, buf)   -- to find where a file lives
 *   upper_path(path, buf)     -- to build an upper-layer path
 *   lower_path(path, buf)     -- to build a lower-layer path
 */

#ifndef MINI_UNIONFS_H
#define MINI_UNIONFS_H

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <limits.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------
 * Global state
 * Allocated once in main(), passed into fuse_main() as
 * private_data. Retrieved in any callback via UNIONFS_DATA.
 * ------------------------------------------------------------- */
struct mini_unionfs_state {
    char *lower_dir;    /* read-only base layer   (e.g. ./test/lower) */
    char *upper_dir;    /* read-write active layer (e.g. ./test/upper) */
};

/*
 * UNIONFS_DATA
 * Retrieves the global state from FUSE's per-request context.
 * Safe to call from any FUSE callback.
 */
#define UNIONFS_DATA \
    ((struct mini_unionfs_state *) fuse_get_context()->private_data)

/* ---------------------------------------------------------------
 * Path helpers (implemented in mini_unionfs.c by Member 1)
 * All write into caller-supplied buffers of size PATH_MAX.
 * ------------------------------------------------------------- */

int  resolve_path(const char *path, char *resolved);
void upper_path(const char *path, char *out);
void lower_path(const char *path, char *out);
void whiteout_path(const char *path, char *out);
int  is_whiteout_name(const char *filename);

/*
 Directory operations (Member 3)
 Implemented in dir_ops.c
 */

int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t offset, struct fuse_file_info *fi);

int unionfs_mkdir(const char *path, mode_t mode);

int unionfs_rmdir(const char *path);

#endif /* MINI_UNIONFS_H */