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
 *
 * These four functions are the foundation that every other
 * team member builds on. Understand them before writing any
 * FUSE callback.
 * ============================================================= */

/*
 * upper_path()
 * Concatenates upper_dir + virtual path -> real on-disk path.
 *
 * Does NOT check existence. Use when you intend to create a
 * new file in the upper layer (Member 2 CoW, Member 4 create).
 *
 * Example:
 *   upper_path("/config.txt", buf)
 *   buf == "/abs/upper/config.txt"
 */
void upper_path(const char *path, char *out)
{
    snprintf(out, PATH_MAX, "%s%s", UNIONFS_DATA->upper_dir, path);
}

/*
 * lower_path()
 * Concatenates lower_dir + virtual path -> real on-disk path.
 */
void lower_path(const char *path, char *out)
{
    snprintf(out, PATH_MAX, "%s%s", UNIONFS_DATA->lower_dir, path);
}

/*
 * whiteout_path()
 * Builds the whiteout marker path for a given virtual path.
 *
 * A whiteout file signals "this file was deleted from the lower
 * layer". It lives in the upper layer beside the deleted file:
 *   virtual  /foo/bar.txt
 *   whiteout upper_dir/foo/.wh.bar.txt
 *
 * Used by:
 *   - resolve_path() to detect deletions
 *   - Member 4's unionfs_unlink() to create markers
 */
void whiteout_path(const char *path, char *out)
{
    /*
     * We need to split path into directory and filename parts
     * because the whiteout file sits in the same directory as
     * the original, with ".wh." prepended to the filename.
     *
     * dirname/basename modify their argument, so we copy first.
     */
    char tmp_dir[PATH_MAX];
    char tmp_base[PATH_MAX];
    strncpy(tmp_dir,  path, PATH_MAX - 1);
    strncpy(tmp_base, path, PATH_MAX - 1);
    tmp_dir[PATH_MAX - 1]  = '\0';
    tmp_base[PATH_MAX - 1] = '\0';

    char *dir  = dirname(tmp_dir);    /* e.g. "/foo"     */
    char *base = basename(tmp_base);  /* e.g. "bar.txt"  */

    /*
     * Edge case: path == "/" -> dirname returns "/", basename
     * returns "/". Guard against constructing "/.wh./" which
     * would be nonsensical. resolve_path() already returns
     * -ENOENT for "/" so this path is never reached in practice,
     * but we protect it here for correctness.
     */
    if (strcmp(base, "/") == 0 || strcmp(base, ".") == 0) {
        snprintf(out, PATH_MAX, "%s/.wh.%s", UNIONFS_DATA->upper_dir, base);
        return;
    }

    snprintf(out, PATH_MAX, "%s%s/.wh.%s",
             UNIONFS_DATA->upper_dir, dir, base);
}

/*
 * is_whiteout_name()
 * Returns 1 if `filename` is a whiteout marker, 0 otherwise.
 *
 * Used by Member 3's readdir to hide whiteout entries from the
 * merged directory listing shown to the user.
 *
 * Example:
 *   is_whiteout_name(".wh.config.txt") == 1
 *   is_whiteout_name("config.txt")     == 0
 */
int is_whiteout_name(const char *filename)
{
    return strncmp(filename, ".wh.", 4) == 0;
}

/*
 * resolve_path()
 *
 * THE central function. Maps a virtual path (as the user sees it)
 * to a real on-disk path, implementing the three-layer lookup:
 *
 *   Step 1 — Whiteout check
 *     If upper_dir contains a .wh.<filename> marker, the file
 *     was deliberately deleted. Return -ENOENT immediately.
 *
 *   Step 2 — Upper layer
 *     If the file exists in upper_dir, return that path.
 *     The upper layer always shadows the lower layer.
 *
 *   Step 3 — Lower layer
 *     If the file exists in lower_dir, return that path.
 *
 *   Step 4 — Not found
 *     Return -ENOENT.
 *
 * Parameters:
 *   path     - virtual path, e.g. "/config.txt" or "/subdir/a.txt"
 *   resolved - output buffer, must be PATH_MAX bytes
 *
 * Returns:
 *   0       on success; `resolved` holds the real path
 *   -ENOENT file is whiteout'd, or simply does not exist
 *
 * Called by: getattr (Member 1), open/read/write (Member 2),
 *            readdir (Member 3), unlink/create (Member 4)
 */
int resolve_path(const char *path, char *resolved)
{
    struct stat st;

    /* ----------------------------------------------------------
     * Step 1: Whiteout check
     * Build upper_dir/dir/.wh.filename and stat it.
     * If it exists, the file was deleted — report ENOENT.
     * ---------------------------------------------------------- */
    char wh[PATH_MAX];
    whiteout_path(path, wh);
    if (lstat(wh, &st) == 0) {
        return -ENOENT;
    }

    /* ----------------------------------------------------------
     * Step 2: Upper layer
     * ---------------------------------------------------------- */
    char up[PATH_MAX];
    upper_path(path, up);
    if (lstat(up, &st) == 0) {
        strncpy(resolved, up, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return 0;
    }

    /* ----------------------------------------------------------
     * Step 3: Lower layer
     * ---------------------------------------------------------- */
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
 * SECTION 3 — getattr (Member 1's FUSE callback)
 *
 * FUSE calls getattr() before almost every other operation —
 * it is the equivalent of stat(). If this returns wrong data,
 * nothing else works correctly.
 *
 * Our job: resolve the virtual path and call lstat() on the
 * real file. If it's whiteout'd or missing, return -ENOENT.
 * ============================================================= */

static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void) fi;  /* unused in our implementation */

    /*
     * Zero the stat buffer first. FUSE may inspect fields we
     * don't explicitly set (e.g. st_dev, st_blksize), and
     * leaving them uninitialised can cause subtle bugs.
     */
    memset(stbuf, 0, sizeof(struct stat));

    /*
     * Special case: the root of the mount point.
     * resolve_path("/") resolves to upper_dir itself, which is
     * correct — but we handle it explicitly to be safe and to
     * always present the mount root as a readable directory.
     */
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    /* Resolve the virtual path to a real on-disk path */
    char rpath[PATH_MAX];
    int res = resolve_path(path, rpath);
    if (res != 0) {
        return res;     /* -ENOENT: whiteout'd or not found */
    }

    /* Stat the real file and copy result into stbuf */
    if (lstat(rpath, stbuf) == -1) {
        return -errno;
    }

    return 0;
}


/* =============================================================
 * SECTION 3B — Member 2: File Access & Copy-on-Write
 *
 * This section implements all file access operations:
 *   - ensure_upper_dir_exists()  helper: mkdir -p for upper layer
 *   - cow_copy_to_upper()        helper: copies file lower -> upper
 *   - unionfs_open()             FUSE callback: open a file
 *   - unionfs_read()             FUSE callback: read file contents
 *   - unionfs_write()            FUSE callback: write with CoW
 *   - unionfs_release()          FUSE callback: close a file
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
 *           e.g. "/home/shimona/mini-unionfs/test/upper/subdir/foo.txt"
 *
 * Returns: 0 on success, -errno on failure
 * --------------------------------------------------------------- */
static int ensure_upper_dir_exists(const char *upath)
{
    /* Work on a mutable copy since dirname() modifies its input */
    char tmp[PATH_MAX];
    strncpy(tmp, upath, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';

    /* Get parent directory of the target file */
    char *parent = dirname(tmp);

    char build[PATH_MAX];
    strncpy(build, parent, PATH_MAX - 1);
    build[PATH_MAX - 1] = '\0';

    /*
     * Walk path from root, calling mkdir at each '/' boundary.
     * Skip leading '/' since root always exists.
     */
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

    /* Create the final directory component */
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
    lower_path(path, lo);   /* real path in lower layer */
    upper_path(path, up);   /* real path in upper layer */

    /* Step 2: ensure upper's parent directory exists */
    int ret = ensure_upper_dir_exists(up);
    if (ret != 0)
        return ret;

    /* Step 3: open the source file in lower layer */
    int src_fd = open(lo, O_RDONLY);
    if (src_fd == -1)
        return -errno;

    /* Step 4: stat the source to preserve permissions */
    struct stat st;
    if (fstat(src_fd, &st) == -1) {
        int err = errno;
        close(src_fd);
        return -err;
    }

    /* Step 5: create destination file in upper layer */
    int dst_fd = open(up, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd == -1) {
        int err = errno;
        close(src_fd);
        return -err;
    }

    /* Step 6: copy data in 64 KiB chunks */
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

    /* Step 7: close both fds */
    close(src_fd);
    close(dst_fd);
    return 0;
}


/* =============================================================
 * SECTION 4 — fuse_operations table
 *
 * This is the vtable that libfuse uses to dispatch kernel
 * requests to our callbacks.
 *
 * Member 1 owns: .getattr
 * Member 2 adds: .open, .read, .write
 * Member 3 adds: .readdir
 * Member 4 adds: .create, .unlink, .mkdir, .rmdir
 *
 * To register your callback, add a line here:
 *   .open = unionfs_open,
 * and implement unionfs_open() in this file.
 * ============================================================= */

static struct fuse_operations unionfs_oper = {
    /* -- Member 1 -- */
    .getattr    = unionfs_getattr,

    /* -- MEMBER 2: uncomment and implement these -- */
    /* .open    = unionfs_open,   */
    /* .read    = unionfs_read,   */
    /* .write   = unionfs_write,  */

    /* -- MEMBER 3: uncomment and implement this -- */
    /* .readdir = unionfs_readdir, */

    /* -- MEMBER 4: uncomment and implement these -- */
    /* .create  = unionfs_create, */
    /* .unlink  = unionfs_unlink, */
    /* .mkdir   = unionfs_mkdir,  */
    /* .rmdir   = unionfs_rmdir,  */
};


/* =============================================================
 * SECTION 5 — main() : Filesystem Initialization
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