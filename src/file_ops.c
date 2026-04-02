/*
 * file_ops.c
 *
 * Member 4 — File Creation & Whiteout-Based Deletion
 * Team 6, Mini-UnionFS
 *
 * Implements:
 *   unionfs_create()  — creates new files in upper layer only
 *   unionfs_unlink()  — deletes files using whiteout markers
 *
 * Uses helpers from mini_unionfs.c (Member 1):
 *   upper_path(path, out)        — builds upper layer absolute path
 *   lower_path(path, out)        — builds lower layer absolute path
 *   whiteout_path(path, out)     — builds .wh.<filename> path in upper
 *   is_whiteout_name(filename)   — checks if a name starts with ".wh."
 *   UNIONFS_DATA                 — macro to access global state
 *
 * Whiteout convention (matches Member 3's dir_ops.c):
 *   Deleting "/foo.txt" creates upper/.wh.foo.txt
 *   Member 3's readdir() already checks for and hides these markers.
 */

#include "mini_unionfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>       /* dirname(), basename() */
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>


/* ================================================================
 * SECTION 1 — INTERNAL HELPER
 * ================================================================ */

/*
 * ensure_upper_parent_exists()
 * ----------------------------
 * Creates the parent directory chain for a file in the upper layer.
 * Works like "mkdir -p" — walks each segment and creates it if missing.
 *
 * Why this is needed:
 *   If a user creates /docs/notes/todo.txt, the folder /docs/notes/
 *   must exist in the upper layer before we can create the file there.
 *   It may only exist in lower (or not at all), so we create it here.
 *
 * Note: Member 1 has a similar static helper in mini_unionfs.c but it
 * is static so we cannot call it from here. This is our own copy.
 *
 * Parameters:
 *   upper_file_path — full absolute path of the FILE in the upper layer
 *                     e.g. /home/user/test/upper/docs/notes/todo.txt
 *
 * Returns:
 *    0        on success (parent dirs exist or were created)
 *   -errno    on failure
 */
static int ensure_upper_parent_exists(const char *upper_file_path)
{
    /* dirname() modifies the string so we need a mutable copy */
    char tmp[PATH_MAX];
    strncpy(tmp, upper_file_path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';

    char *parent = dirname(tmp);  /* e.g. /home/user/test/upper/docs/notes */

    /* If parent already exists, nothing to do */
    struct stat st;
    if (stat(parent, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;

    /* Walk segment by segment and mkdir each one */
    char build[PATH_MAX];
    strncpy(build, parent, PATH_MAX - 1);
    build[PATH_MAX - 1] = '\0';

    /* Start at index 1 to skip the leading '/' */
    for (char *p = build + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';  /* null-terminate at this slash */

            if (mkdir(build, 0755) == -1 && errno != EEXIST) {
                fprintf(stderr,
                    "[file_ops] ensure_upper_parent_exists: "
                    "mkdir('%s') failed: %s\n",
                    build, strerror(errno));
                return -errno;
            }

            *p = '/';   /* restore the slash */
        }
    }

    /* Create the final deepest directory */
    if (mkdir(build, 0755) == -1 && errno != EEXIST) {
        fprintf(stderr,
            "[file_ops] ensure_upper_parent_exists: "
            "final mkdir('%s') failed: %s\n",
            build, strerror(errno));
        return -errno;
    }

    return 0;
}


/* ================================================================
 * SECTION 2 — FILE CREATION
 * ================================================================ */

/*
 * unionfs_create()
 * ----------------
 * FUSE hook — called when a user creates a new file.
 * Triggered by: touch, open(O_CREAT), creat(), vim new file, etc.
 *
 * What we must do:
 *   1. Ensure parent directory exists in upper layer
 *   2. Remove stale whiteout if this filename was previously deleted
 *   3. Create the file in the upper layer only (never in lower)
 *   4. Set correct permissions (mode) via fchmod()
 *   5. Set correct ownership (uid/gid) via fchown()
 *   6. Store the open fd in fi->fh for Member 2's read/write to reuse
 *
 * Parameters (standard FUSE):
 *   path  — virtual path,    e.g. "/docs/todo.txt"
 *   mode  — permission bits, e.g. 0644
 *   fi    — FUSE file info;  we populate fi->fh with the real fd
 *
 * Returns:
 *    0      on success
 *   -errno  on failure
 */
int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char upath[PATH_MAX];
    upper_path(path, upath);
    /* upath is now e.g. /home/user/test/upper/docs/todo.txt */

    printf("[create] path='%s'  upper='%s'\n", path, upath);

    /* ----------------------------------------------------------
     * STEP 1: Create parent directory chain in upper layer.
     *
     * Example: creating /docs/todo.txt when upper/docs/ doesn't
     * exist yet — we must create it before touching the file.
     * ---------------------------------------------------------- */
    int ret = ensure_upper_parent_exists(upath);
    if (ret != 0) {
        fprintf(stderr,
            "[create] Failed to create parent dirs for '%s': %s\n",
            upath, strerror(-ret));
        return ret;
    }

    /* ----------------------------------------------------------
     * STEP 2: Remove stale whiteout marker if it exists.
     *
     * Scenario: user ran `rm docs/todo.txt` before — this created
     * upper/docs/.wh.todo.txt. Now user runs `touch docs/todo.txt`
     * again. We must remove the whiteout first so the new file
     * is visible in the merged view (Member 3's readdir checks this).
     * ---------------------------------------------------------- */
    char wpath[PATH_MAX];
    whiteout_path(path, wpath);
    /* wpath = e.g. /home/user/test/upper/docs/.wh.todo.txt */

    if (access(wpath, F_OK) == 0) {
        /* Whiteout exists — remove it */
        if (unlink(wpath) != 0) {
            fprintf(stderr,
                "[create] Warning: could not remove whiteout '%s': %s\n",
                wpath, strerror(errno));
            /* Non-fatal: we continue, file will still be created */
        } else {
            printf("[create] Removed stale whiteout: %s\n", wpath);
        }
    }

    /* ----------------------------------------------------------
     * STEP 3: Create the file in the upper layer.
     *
     * fi->flags holds the caller's open flags (O_RDWR, O_TRUNC…)
     * We OR in O_CREAT to guarantee creation.
     * mode is passed as the permission argument to open().
     * ---------------------------------------------------------- */
    int fd = open(upath, fi->flags | O_CREAT, mode);
    if (fd == -1) {
        int err = errno;
        fprintf(stderr,
            "[create] open() failed for '%s': %s\n",
            upath, strerror(err));
        return -err;
    }

    /* ----------------------------------------------------------
     * STEP 4: Set exact permissions via fchmod().
     *
     * open() with O_CREAT applies mode but it gets masked by the
     * process umask. fchmod() sets the EXACT bits we were asked for.
     * e.g. if mode=0644, the file will be exactly -rw-r--r--
     * ---------------------------------------------------------- */
    if (fchmod(fd, mode) != 0) {
        fprintf(stderr,
            "[create] Warning: fchmod() failed for '%s': %s\n",
            upath, strerror(errno));
        /* Non-fatal — file is still usable */
    }

    /* ----------------------------------------------------------
     * STEP 5: Set correct ownership (uid and gid).
     *
     * fuse_get_context() gives us the uid/gid of the process that
     * called create() (e.g. the user who ran `touch`).
     * Without this, the file would be owned by whoever mounted FUSE.
     * ---------------------------------------------------------- */
    struct fuse_context *ctx = fuse_get_context();
    if (fchown(fd, ctx->uid, ctx->gid) != 0) {
        fprintf(stderr,
            "[create] Warning: fchown() failed for '%s': %s\n",
            upath, strerror(errno));
        /* Non-fatal */
    }

    /* ----------------------------------------------------------
     * STEP 6: Store fd in fi->fh for Member 2's read/write.
     *
     * Member 2's unionfs_read() and unionfs_write() use fi->fh
     * directly (via pread/pwrite) without re-opening the file.
     * This matches exactly how Member 2 does it in unionfs_open().
     * ---------------------------------------------------------- */
    fi->fh = (uint64_t) fd;

    printf("[create] SUCCESS: '%s' created (mode=%o uid=%d gid=%d fd=%d)\n",
           upath, mode, ctx->uid, ctx->gid, fd);

    return 0;  /* success */
}


/* ================================================================
 * SECTION 3 — WHITEOUT-BASED DELETION
 * ================================================================ */

/*
 * unionfs_unlink()
 * ----------------
 * FUSE hook — called when a user deletes a file.
 * Triggered by: rm, unlink(), file managers, etc.
 *
 * The core problem:
 *   Lower layer is READ-ONLY — we can NEVER delete files from it.
 *   Instead we create a whiteout marker in the upper layer.
 *   Member 3's readdir() sees the whiteout and hides the file
 *   from the merged view, making it appear deleted to the user.
 *
 * Whiteout file format (matches Member 3's dir_ops.c):
 *   Deleting "/docs/todo.txt" creates upper/docs/.wh.todo.txt
 *   (Member 1's whiteout_path() builds this for us)
 *
 * Three cases:
 *   Case A — file only in upper:       delete directly, no whiteout needed
 *   Case B — file only in lower:       create whiteout in upper
 *   Case C — file in both layers:      delete upper copy + create whiteout
 *
 * Parameters:
 *   path — virtual path, e.g. "/docs/todo.txt"
 *
 * Returns:
 *    0      on success
 *   -errno  on failure
 */
int unionfs_unlink(const char *path)
{
    char upath[PATH_MAX];
    char lpath[PATH_MAX];
    upper_path(path, upath);
    lower_path(path, lpath);

    printf("[unlink] path='%s'\n", path);

    /* Check which layers contain this file */
    struct stat ust, lst;
    int in_upper = (stat(upath, &ust) == 0);
    int in_lower = (stat(lpath, &lst) == 0);

    /* File must exist somewhere */
    if (!in_upper && !in_lower) {
        fprintf(stderr, "[unlink] '%s' not found in either layer\n", path);
        return -ENOENT;
    }

    /* ----------------------------------------------------------
     * CASE A: File exists ONLY in the upper layer.
     *
     * Safe to delete directly. No lower copy means nothing to hide,
     * so no whiteout is needed.
     * ---------------------------------------------------------- */
    if (in_upper && !in_lower) {
        printf("[unlink] Case A: upper-only file, deleting directly.\n");

        if (unlink(upath) != 0) {
            int err = errno;
            fprintf(stderr,
                "[unlink] unlink('%s') failed: %s\n",
                upath, strerror(err));
            return -err;
        }

        printf("[unlink] SUCCESS: deleted upper-only file '%s'\n", upath);
        return 0;
    }

    /* ----------------------------------------------------------
     * CASE C: File exists in BOTH layers.
     *
     * Delete the upper copy first — otherwise the upper file would
     * keep showing up in the merged view despite the whiteout.
     * Then fall through to whiteout creation below (same as Case B).
     * ---------------------------------------------------------- */
    if (in_upper && in_lower) {
        printf("[unlink] Case C: file in both layers, "
               "removing upper copy first.\n");

        if (unlink(upath) != 0) {
            int err = errno;
            fprintf(stderr,
                "[unlink] Failed to remove upper copy '%s': %s\n",
                upath, strerror(err));
            return -err;
        }
        printf("[unlink] Removed upper copy: '%s'\n", upath);
    }

    /* ----------------------------------------------------------
     * CASE B (and fall-through from Case C):
     * File exists in lower layer — create whiteout marker.
     *
     * whiteout_path() from Member 1 builds the correct path:
     *   e.g. /home/user/test/upper/docs/.wh.todo.txt
     * ---------------------------------------------------------- */
    printf("[unlink] %s: creating whiteout for lower-layer file.\n",
           in_upper ? "Case C" : "Case B");

    /* Ensure parent directory exists in upper before creating whiteout */
    int ret = ensure_upper_parent_exists(upath);
    if (ret != 0) {
        fprintf(stderr,
            "[unlink] Could not create upper parent dir for whiteout: %s\n",
            strerror(-ret));
        return ret;
    }

    /* Build whiteout path using Member 1's helper */
    char wpath[PATH_MAX];
    whiteout_path(path, wpath);
    /* wpath = e.g. /home/user/test/upper/docs/.wh.todo.txt */

    /* Create the whiteout — an empty file, mode 0000 */
    int wh_fd = open(wpath, O_CREAT | O_WRONLY | O_TRUNC, 0000);
    if (wh_fd == -1) {
        int err = errno;
        fprintf(stderr,
            "[unlink] Failed to create whiteout '%s': %s\n",
            wpath, strerror(err));
        return -err;
    }
    close(wh_fd);

    printf("[unlink] SUCCESS: whiteout created at '%s'\n", wpath);
    printf("[unlink] '%s' now appears deleted in the merged view.\n", path);

    return 0;  /* success */
}
