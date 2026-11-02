/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "husk-tl-apk-path.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

bool tl_apk_install_path(const char *apk, const char *data, char *out, size_t size)
{
    if (!apk || !data || !out || !size) { errno = EINVAL; return false; }
    out[0] = 0;
    char source[PATH_MAX], dir[PATH_MAX], installed[PATH_MAX], tempdir[PATH_MAX], temp[PATH_MAX];
    struct stat src, dst;
    if (!realpath(apk, source) || stat(source, &src) != 0) return false;
    if (!S_ISREG(src.st_mode)) { errno = EINVAL; return false; }
    if (snprintf(dir, sizeof(dir), "%s/.husk-package", data) >= (int)sizeof(dir) ||
        snprintf(installed, sizeof(installed), "%s/base.apk", dir) >= (int)sizeof(installed) ||
        snprintf(tempdir, sizeof(tempdir), "%s/.install.XXXXXX", dir) >= (int)sizeof(tempdir) ||
        snprintf(temp, sizeof(temp), "%s/base.apk", tempdir) >= (int)sizeof(temp) ||
        snprintf(out, size, "%s", installed) >= (int)size) {
        out[0] = 0; errno = ENAMETOOLONG; return false;
    }
    out[0] = 0;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return false;
    if (lstat(dir, &dst) != 0) return false;
    if (!S_ISDIR(dst.st_mode)) { errno = ENOTDIR; return false; }
    if (lstat(installed, &dst) == 0) {
        if (!S_ISREG(dst.st_mode)) { errno = EEXIST; return false; }
        if (src.st_dev == dst.st_dev && src.st_ino == dst.st_ino) {
            snprintf(out, size, "%s", installed);
            return true;
        }
    } else if (errno != ENOENT) return false;

    /* A hard link preserves the exact signed APK bytes and costs no second copy.
     * Unlike a symlink, realpath(base.apk) keeps the installed .apk spelling.
     * Replacing an old managed alias is atomic; a failure keeps it intact. */
    if (!mkdtemp(tempdir)) return false;
    snprintf(temp, sizeof(temp), "%s/base.apk", tempdir);
    bool ok = link(source, temp) == 0;
    if (ok) ok = rename(temp, installed) == 0;
    int error = errno;
    if (!ok) unlink(temp);
    rmdir(tempdir);
    if (!ok) { errno = error; return false; }
    snprintf(out, size, "%s", installed);
    return true;
}
