/*
 * mini_unionfs.c
 *
 * ╔══════════════════════════════════════════════════════╗
 * ║  Member 1 owns everything in this file:             ║
 * ║    - Global state & FUSE context macro  (Section 1) ║
 * ║    - Path helpers & resolve_path()      (Section 2) ║
 * ║    - unionfs_getattr()                  (Section 3) ║
 * ║    - fuse_operations stub table         (Section 4) ║
 * ║    - main() / argument parsing          (Section 5) ║
 * ╚══════════════════════════════════════════════════════╝
 *
 * Members 2-4: search for "MEMBER 2", "MEMBER 3", "MEMBER 4"
 * to find the exact slots where your callbacks plug in.
 * Every callback you write should start with resolve_path()
 * or upper_path() — both are declared in mini_unionfs.h.
 */

#include "mini_unionfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>     /* dirname(), basename() */
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>


/* =============================================================
 * SECTION 1 — Global State
 *
 * struct mini_unionfs_state is defined in mini_unionfs.h.
 * The UNIONFS_DATA macro retrieves it inside any FUSE callback:
 *
 *   const char *upper = UNIONFS_DATA->upper_dir;
 *   const char *lower = UNIONFS_DATA->lower_dir;
 * ============================================================= */


/* =============================================================
 * SECTION 2 — Path Helpers
 * ============================================================= */

void upper_path(const char *path, char *out)
{
    snprintf(out, PATH_MAX, "%s%s", UNIONFS_DATA->upper_dir, path);
}

void lower_path(const char *path, char *out)
{
    snprintf(out, PATH_MAX, "%s%s", UNIONFS_DATA->lower_dir, path);
}

void whiteout_path(const char *path, char *out)
{
    char tmp_dir[PATH_MAX];
    char tmp_base[PATH_MAX];
    strncpy(tmp_dir,  path, PATH_MAX - 1);
    strncpy(tmp_base, path, PATH_MAX - 1);
    tmp_dir[PATH_MAX - 1]  = '\0';
    tmp_base[PATH_MAX - 1] = '\0';

    char *dir  = dirname(tmp_dir);
    char *base = basename(tmp_base);

    if (strcmp(base, "/") == 0 || strcmp(base, ".") == 0) {
        snprintf(out, PATH_MAX, "%s/.wh.%s", UNIONFS_DATA->upper_dir, base);
        return;
    }

    if (strcmp(dir, "/") == 0)
    snprintf(out, PATH_MAX, "%s/.wh.%s",
             UNIONFS_DATA->upper_dir, base);
    else
    snprintf(out, PATH_MAX, "%s%s/.wh.%s",
             UNIONFS_DATA->upper_dir, dir, base);
}

int is_whiteout_name(const char *filename)
{
    return strncmp(filename, ".wh.", 4) == 0;
}

int resolve_path(const char *path, char *resolved)
{
    struct stat st;

    char wh[PATH_MAX];
    whiteout_path(path, wh);
    if (lstat(wh, &st) == 0) {
        return -ENOENT;
    }

    char up[PATH_MAX];
    upper_path(path, up);
    if (lstat(up, &st) == 0) {
        strncpy(resolved, up, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return 0;
    }

    char lo[PATH_MAX];
    lower_path(path, lo);
    if (lstat(lo, &st) == 0) {
        strncpy(resolved, lo, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return 0;
    }

    return -ENOENT;
}


/* =============================================================
 * SECTION 3 — getattr (Member 1)
 * ============================================================= */

static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void) fi;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    char rpath[PATH_MAX];
    int res = resolve_path(path, rpath);
    if (res != 0)
        return res;

    if (lstat(rpath, stbuf) == -1)
        return -errno;

    return 0;
}


/* =============================================================
 * SECTION 3B — Member 2: File Access & Copy-on-Write
 * ============================================================= */

/* ---------------------------------------------------------------
 * HELPER — ensure_upper_dir_exists()
 *
 * When copying a file to the upper layer, its parent directory
 * must already exist. For example, to CoW /subdir/foo.txt,
 * upper/subdir/ must exist first.
 *
 * This walks the path component by component and calls mkdir()
 * at each level — exactly like "mkdir -p". EEXIST is not an error.
 *
 * Parameters:
 *   upath - full upper-layer path of the FILE to be created
 *
 * Returns: 0 on success, -errno on failure
 * --------------------------------------------------------------- */
static int ensure_upper_dir_exists(const char *upath)
{
    char tmp[PATH_MAX];
    strncpy(tmp, upath, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';

    char *parent = dirname(tmp);

    char build[PATH_MAX];
    strncpy(build, parent, PATH_MAX - 1);
    build[PATH_MAX - 1] = '\0';

    char *p = build + 1;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(build, 0755) == -1 && errno != EEXIST)
                return -errno;
            *p = '/';
        }
        p++;
    }

    if (mkdir(build, 0755) == -1 && errno != EEXIST)
        return -errno;

    return 0;
}

/* ---------------------------------------------------------------
 * HELPER — cow_copy_to_upper()
 *
 * Copy-on-Write: copies a file from the read-only lower layer
 * into the read-write upper layer before any modification.
 * This ensures the lower original is NEVER touched.
 *
 * Steps:
 *   1. Build lower and upper real paths from virtual path
 *   2. Ensure parent directory exists in upper layer
 *   3. Open lower file for reading
 *   4. Stat lower file to preserve its permissions
 *   5. Create upper file with same permissions
 *   6. Copy content in 64 KiB chunks
 *   7. Close both file descriptors
 *
 * Parameters:
 *   path - virtual path, e.g. "/hello.txt"
 *
 * Returns: 0 on success, -errno on failure
 * --------------------------------------------------------------- */
static int cow_copy_to_upper(const char *path)
{
    char lo[PATH_MAX], up[PATH_MAX];
    lower_path(path, lo);
    upper_path(path, up);

    int ret = ensure_upper_dir_exists(up);
    if (ret != 0)
        return ret;

    int src_fd = open(lo, O_RDONLY);
    if (src_fd == -1)
        return -errno;

    struct stat st;
    if (fstat(src_fd, &st) == -1) {
        int err = errno;
        close(src_fd);
        return -err;
    }

    int dst_fd = open(up, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd == -1) {
        int err = errno;
        close(src_fd);
        return -err;
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(dst_fd, buf + written, n - written);
            if (w == -1) {
                int err = errno;
                close(src_fd);
                close(dst_fd);
                return -err;
            }
            written += w;
        }
    }

    close(src_fd);
    close(dst_fd);
    return 0;
}

/* ---------------------------------------------------------------
 * unionfs_open()
 *
 * Called by FUSE when a user opens a file (e.g. cat, echo, vim).
 *
 * Logic:
 *   1. Resolve virtual path to find where the file lives
 *   2. If write is requested AND file is in lower layer:
 *      trigger Copy-on-Write so we always write to upper
 *   3. Open the real file and store fd in fi->fh so
 *      read/write/release can use it without re-resolving
 *
 * fi->fh stores the real fd (cast to uint64_t).
 * This is the standard FUSE pattern for per-file state.
 *
 * Returns: 0 on success, -errno on failure
 * --------------------------------------------------------------- */
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    /* Step 1: find where the file currently lives */
    char rpath[PATH_MAX];
    int ret = resolve_path(path, rpath);
    if (ret != 0)
        return ret;

    /* Step 2: if any write flag is set, ensure file is in upper */
    int write_requested = (fi->flags & O_ACCMODE) != O_RDONLY;

    if (write_requested) {
        const char *upper_dir = UNIONFS_DATA->upper_dir;

        /*
         * Check if file is in lower by seeing if rpath
         * does NOT start with upper_dir.
         */
        int in_lower = (strncmp(rpath, upper_dir, strlen(upper_dir)) != 0);

        if (in_lower) {
            /*
             * File only exists in lower (read-only) layer.
             * Copy it to upper first (Copy-on-Write),
             * then open the upper copy for writing.
             */
            ret = cow_copy_to_upper(path);
            if (ret != 0)
                return ret;

            /* Point rpath to the new upper copy */
            upper_path(path, rpath);
        }
    }

    /* Step 3: open the real file with original flags */
    int fd = open(rpath, fi->flags);
    if (fd == -1)
        return -errno;

    /* Store fd in fi->fh for use by read/write/release */
    fi->fh = (uint64_t) fd;
    return 0;
}

/* ---------------------------------------------------------------
 * unionfs_read()                               [COMMIT 4]
 *
 * Called by FUSE when a user reads from an open file
 * (e.g. cat, fread, read()).
 *
 * Because unionfs_open() already resolved the correct layer
 * (upper or lower) and stored the real fd in fi->fh, we simply
 * delegate to pread() here — no path resolution needed.
 *
 * pread() reads 'size' bytes starting at 'offset' without
 * changing the file's current seek position.
 *
 * Parameters:
 *   path   - virtual path (unused; fd in fi->fh is sufficient)
 *   buf    - output buffer to fill
 *   size   - number of bytes requested
 *   offset - byte offset to start reading from
 *   fi     - fi->fh holds the real fd from unionfs_open()
 *
 * Returns: number of bytes actually read, or -errno on failure
 * --------------------------------------------------------------- */
static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void) path;    /* fd in fi->fh is all we need */

    ssize_t res = pread((int) fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;

    return (int) res;
}

/* ---------------------------------------------------------------
 * unionfs_write()                              [COMMIT 4]
 *
 * Called by FUSE when a user writes to an open file
 * (e.g. echo "x" > file, fwrite(), write()).
 *
 * unionfs_open() already performed Copy-on-Write if the file
 * was in the lower layer, so fi->fh ALWAYS points to a file
 * in the upper (writable) layer by the time we get here.
 *
 * We delegate to pwrite() which writes at the given offset
 * without changing the file's seek position.
 *
 * Parameters:
 *   path   - virtual path (unused; fd in fi->fh is sufficient)
 *   buf    - data to write
 *   size   - number of bytes to write
 *   offset - byte offset to write at
 *   fi     - fi->fh holds the real fd from unionfs_open()
 *
 * Returns: number of bytes actually written, or -errno on failure
 * --------------------------------------------------------------- */
static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) path;    /* fd in fi->fh is all we need */

    ssize_t res = pwrite((int) fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;

    return (int) res;
}

/* ---------------------------------------------------------------
 * unionfs_release()                            [COMMIT 4]
 *
 * Called by FUSE when the last reference to an open file is
 * closed (i.e. the final close() call from userspace).
 *
 * We simply close the real fd that unionfs_open() stored in
 * fi->fh to avoid leaking file descriptors.
 *
 * Note: FUSE ignores the return value of release(), but we
 * return 0 by convention.
 *
 * Parameters:
 *   path - virtual path (unused)
 *   fi   - fi->fh holds the real fd to close
 *
 * Returns: 0 always
 * --------------------------------------------------------------- */
static int unionfs_release(const char *path, struct fuse_file_info *fi)
{
    (void) path;

    close((int) fi->fh);
    return 0;
}


/* =============================================================
 * SECTION 4 — fuse_operations table
 * ============================================================= */

static struct fuse_operations unionfs_oper = {
    /* -- Member 1: -- */
    .getattr    = unionfs_getattr,

    /* -- MEMBER 2: -- */
    .open       = unionfs_open,
    .read       = unionfs_read,
    .write      = unionfs_write,
    .release    = unionfs_release,

    /* -- MEMBER 3: uncomment and implement this -- */
    /* .readdir = unionfs_readdir, */

    /* -- MEMBER 4: uncomment and implement these -- */
    .create  = unionfs_create, 
    .unlink  = unionfs_unlink,
    /* .mkdir   = unionfs_mkdir,  */
    /* .rmdir   = unionfs_rmdir,  */
};


/* =============================================================
 * SECTION 5 — main()
 * ============================================================= */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "\nUsage: %s <lower_dir> <upper_dir> <mount_point> [fuse_options]\n"
            "\n"
            "  lower_dir   - read-only base layer  (e.g. ./test/lower)\n"
            "  upper_dir   - read-write layer       (e.g. ./test/upper)\n"
            "  mount_point - where to mount the FS  (e.g. ./test/mnt)\n"
            "\nFUSE options (optional):\n"
            "  -f          run in foreground (recommended for debugging)\n"
            "  -d          enable FUSE debug output\n"
            "  -s          single-threaded mode\n\n",
            argv[0]);
        return 1;
    }

    struct mini_unionfs_state *state =
        malloc(sizeof(struct mini_unionfs_state));

    if (!state) {
        perror("malloc: could not allocate filesystem state");
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir) {
        fprintf(stderr, "Error: lower_dir '%s' not found or inaccessible\n",
                argv[1]);
        free(state);
        return 1;
    }
    if (!state->upper_dir) {
        fprintf(stderr, "Error: upper_dir '%s' not found or inaccessible\n",
                argv[2]);
        free(state->lower_dir);
        free(state);
        return 1;
    }

    fprintf(stderr, "\n[mini_unionfs] starting up\n");
    fprintf(stderr, "  lower (read-only) : %s\n", state->lower_dir);
    fprintf(stderr, "  upper (read-write): %s\n", state->upper_dir);
    fprintf(stderr, "  mount point       : %s\n", argv[3]);
    fprintf(stderr, "  tip: pass -f to run in foreground for easier debugging\n\n");

    for (int i = 1; i < argc - 2; i++) {
        argv[i] = argv[i + 2];
    }
    argc -= 2;

    return fuse_main(argc, argv, &unionfs_oper, state);
}
