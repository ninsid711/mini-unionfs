#include "mini_unionfs.h"

#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

/* ---------------------------------------------------------------
 * Helper: check if a file name is already seen (to avoid duplicates)
 * ------------------------------------------------------------- */
static int already_seen(char **seen, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(seen[i], name) == 0)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------
 * unionfs_readdir
 *
 * This function merges directory contents from:
 *   - Upper layer (priority)
 *   - Lower layer (fallback)
 *
 * Rules:
 *   - Upper overrides lower
 *   - No duplicate entries
 *   - Whiteout files (.wh.*) hide lower files
 * ------------------------------------------------------------- */
int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                   off_t offset, struct fuse_file_info *fi,
                   enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) fi;
    (void) flags;

    DIR *dp;
    struct dirent *entry;

    char upper[PATH_MAX];
    char lower[PATH_MAX];

    upper_path(path, upper);
    lower_path(path, lower);

    /* Track files already added (from upper) */
    char *seen[1024];
    int count = 0;

    /* Add mandatory entries */
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* ===================== READ UPPER ===================== */
    dp = opendir(upper);
    if (dp != NULL) {
        while ((entry = readdir(dp)) != NULL) {

            /* Skip . and .. */
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            /* Skip whiteout files themselves */
            if (is_whiteout_name(entry->d_name))
                continue;

            /* Add to output */
            filler(buf, entry->d_name, NULL, 0, 0);

            /* Track to avoid duplicates later */
            if (count < 1024) {
                char *dup = strdup(entry->d_name);
                if (dup)
                    seen[count++] = dup;
            }
        }
        closedir(dp);
    }

    /* ===================== READ LOWER ===================== */
    dp = opendir(lower);
    if (dp != NULL) {
        while ((entry = readdir(dp)) != NULL) {

            /* Skip . and .. */
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            /* Skip if already in upper */
            if (already_seen(seen, count, entry->d_name))
                continue;

            /* Build full path for whiteout check */
            char temp[PATH_MAX];
            snprintf(temp, PATH_MAX, "%s/%s", path, entry->d_name);

            char wh_full[PATH_MAX];
            whiteout_path(temp, wh_full);

            /* If whiteout exists, skip */
            if (access(wh_full, F_OK) == 0)
                continue;

            /* Add remaining lower files */
            filler(buf, entry->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

    /* Cleanup memory */
    for (int i = 0; i < count; i++) {
        free(seen[i]);
    }

    return 0;
}

/* ---------------------------------------------------------------
 * mkdir: create directory only in upper layer
 * ------------------------------------------------------------- */
int unionfs_mkdir(const char *path, mode_t mode)
{
    char upper[PATH_MAX];
    upper_path(path, upper);

    int res = mkdir(upper, mode);
    if (res == -1)
        return -errno;

    return 0;
}

/* ---------------------------------------------------------------
 * rmdir: remove directory only from upper layer
 * ------------------------------------------------------------- */
int unionfs_rmdir(const char *path)
{
    char upper[PATH_MAX];
    upper_path(path, upper);

    /* If not in upper, treat as not found */
    if (access(upper, F_OK) != 0)
        return -ENOENT;

    int res = rmdir(upper);
    if (res == -1)
        return -errno;

    return 0;
}