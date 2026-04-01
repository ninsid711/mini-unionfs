#include "mini_unionfs.h"
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>

static int already_seen(char **seen, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(seen[i], name) == 0)
            return 1;
    }
    return 0;
}

int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                   off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    DIR *dp;
    struct dirent *entry;

    char upper[PATH_MAX];
    char lower[PATH_MAX];

    upper_path(path, upper);
    lower_path(path, lower);

    char *seen[1024];
    int count = 0;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    /* READ UPPER */
    dp = opendir(upper);
    if (dp != NULL) {
        while ((entry = readdir(dp)) != NULL) {

            if (is_whiteout_name(entry->d_name))
                continue;

            filler(buf, entry->d_name, NULL, 0);

            seen[count++] = strdup(entry->d_name);
        }
        closedir(dp);
    }

    /* READ LOWER */
    dp = opendir(lower);
    if (dp != NULL) {
        while ((entry = readdir(dp)) != NULL) {

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            if (already_seen(seen, count, entry->d_name))
                continue;

            char wh[PATH_MAX];
            whiteout_path(path, wh);

            char wh_full[PATH_MAX];
            snprintf(wh_full, PATH_MAX, "%s/%s", wh, entry->d_name);

            if (access(wh_full, F_OK) == 0)
                continue;

            filler(buf, entry->d_name, NULL, 0);
        }
        closedir(dp);
    }

    for (int i = 0; i < count; i++) {
        free(seen[i]);
    }

    return 0;
}

