#pragma once
/*
 * path_util.h — Platform-safe path manipulation helpers.
 *
 * All returned strings are heap-allocated; free with str_free().
 * Keeps path operations isolated from string utilities (SRP).
 */
#ifndef WISP_PATH_UTIL_H
#define WISP_PATH_UTIL_H

#include "wisp_bool.h"


#ifdef __cplusplus
extern "C" {
#endif

/* True if path exists (file or directory). */
bool  path_exists(const char *path);

/* True if path is a directory. */
bool  path_is_dir(const char *path);

/* Expand leading ~ to %USERPROFILE%.  Caller frees result. */
char *path_expand_tilde(const char *path);

/* Join two path components with a backslash.  Caller frees result. */
char *path_join(const char *base, const char *part);

/* Return heap copy of the basename (last component). */
char *path_basename(const char *path);

/* Return heap copy of the directory part. */
char *path_dirname(const char *path);

/* Ensure all directories in path exist (like mkdir -p).
 * Returns true on success or if already exists. */
bool  path_mkdirs(const char *path);


#ifdef __cplusplus
}
#endif

#endif /* WISP_PATH_UTIL_H */
