/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_APK_PATH_H
#define HUSK_TL_APK_PATH_H
#include <stdbool.h>
#include <stddef.h>

/* Expose the imported base package under Android's installed name, base.apk.
 * The original is never renamed or rewritten. errno describes any failure. */
bool tl_apk_install_path(const char *apk, const char *data, char *out, size_t size);
#endif
