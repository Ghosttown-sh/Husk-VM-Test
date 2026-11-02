/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "husk-tl-apk-path.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *p, const char *bytes)
{
    FILE *f = fopen(p, "wb"); assert(f);
    assert(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes));
    assert(fclose(f) == 0);
}
static void check_file(const char *p, const char *bytes)
{
    char b[100] = {0}; FILE *f = fopen(p, "rb"); assert(f);
    assert(fread(b, 1, sizeof(b), f) == strlen(bytes));
    assert(strcmp(b, bytes) == 0); assert(fclose(f) == 0);
}
int main(void)
{
    char root[] = "/tmp/husk-apk-path.XXXXXX"; assert(mkdtemp(root));
    char previous[PATH_MAX]; assert(getcwd(previous, sizeof(previous)));
    assert(chdir(root) == 0);
    const char *apk = "Visual Maimai Mobile release 1.1.1.aPK";
    assert(mkdir("data", 0700) == 0);
    write_file(apk, "original APK bytes");
    char out[PATH_MAX], resolved[PATH_MAX];
    assert(tl_apk_install_path(apk, "data", out, sizeof(out)));
    assert(strcmp(out, "data/.husk-package/base.apk") == 0);
    struct stat src, dst;
    assert(stat(apk, &src) == 0 && lstat(out, &dst) == 0);
    assert(S_ISREG(dst.st_mode) && src.st_dev == dst.st_dev && src.st_ino == dst.st_ino);
    assert(realpath(out, resolved) && strstr(resolved, "/.husk-package/base.apk"));
    check_file(out, "original APK bytes"); check_file(apk, "original APK bytes");
    assert(tl_apk_install_path(apk, "data", out, sizeof(out))); // repeat launch
    assert(stat(out, &dst) == 0 && dst.st_ino == src.st_ino);
    puts("PASS: mixed-case import gets byte-identical base.apk hard link, preserved by realpath and repeat launch");

    // Simulate an APK update by atomic replacement, without changing the old inode.
    write_file("replacement.apk", "updated APK bytes");
    assert(rename("replacement.apk", apk) == 0);
    assert(tl_apk_install_path(apk, "data", out, sizeof(out)));
    check_file(out, "updated APK bytes"); check_file(apk, "updated APK bytes");
    assert(stat(apk, &src) == 0 && stat(out, &dst) == 0 && src.st_ino == dst.st_ino);
    char small[2] = {1,1};
    assert(!tl_apk_install_path(apk, "data", small, sizeof(small)) && errno == ENAMETOOLONG && !small[0]);
    assert(!tl_apk_install_path("missing.apk", "data", out, sizeof(out)) && errno == ENOENT && !out[0]);
    assert(!tl_apk_install_path("data", "data", out, sizeof(out)) && errno == EINVAL);
    check_file("data/.husk-package/base.apk", "updated APK bytes");
    puts("PASS: changed source replaces only the managed alias; invalid inputs keep the previous APK intact");

    assert(unlink("data/.husk-package/base.apk") == 0);
    assert(symlink("../../Visual Maimai Mobile release 1.1.1.aPK", "data/.husk-package/base.apk") == 0);
    assert(!tl_apk_install_path(apk, "data", out, sizeof(out)) && errno == EEXIST);
    assert(unlink("data/.husk-package/base.apk") == 0);
    assert(rmdir("data/.husk-package") == 0); // proves temporary directories were removed
    assert(symlink("..", "data/.husk-package") == 0);
    assert(!tl_apk_install_path(apk, "data", out, sizeof(out)) && errno == ENOTDIR);
    assert(unlink("data/.husk-package") == 0);
    check_file(apk, "updated APK bytes");
    assert(unlink(apk) == 0 && rmdir("data") == 0);
    assert(chdir(previous) == 0 && rmdir(root) == 0);
    puts("PASS: conflicting symlinks are refused; no source or user data overwritten");
    return 0;
}
