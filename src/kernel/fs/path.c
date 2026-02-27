/* fs/path.c — Path manipulation utilities */
#include "vfs.h"
#include "lib/string.h"

/* ── path_normalize ──────────────────────────────────────────────────────── *
 * Remove redundant slashes, "." components, and resolve ".." components.
 * `out` must be at least VFS_MOUNT_PATH_MAX bytes.
 * Returns 0 on success, -EINVAL if path is not absolute or result overflows.
 */
int path_normalize(const char *in, char *out) {
    if (!in || !out || in[0] != '/') return -EINVAL;

    /* Build normalized output in-place to avoid large stack buffers. */
    size_t out_len = 1;
    out[0] = '/';
    out[1] = '\0';

    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *comp = p;
        size_t clen = 0;
        while (*p && *p != '/') {
            p++;
            clen++;
        }

        /* Reject overlong components instead of truncating/splitting them. */
        if (clen > VFS_NAME_MAX) return -EINVAL;

        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && out[out_len - 1] != '/')
                    out_len--;
                if (out_len > 1) out_len--;
                out[out_len] = '\0';
            }
            continue;
        }

        /* Append "/component" (without duplicate leading slash at root). */
        if (out_len > 1) {
            if (out_len + 1 >= VFS_MOUNT_PATH_MAX) return -EINVAL;
            out[out_len++] = '/';
        }
        if (out_len + clen >= VFS_MOUNT_PATH_MAX) return -EINVAL;
        memcpy(out + out_len, comp, clen);
        out_len += clen;
        out[out_len] = '\0';
    }

    return 0;
}

/* ── path_join ───────────────────────────────────────────────────────────── *
 * Join `base` (absolute) and `rel` (relative or absolute).
 * If `rel` starts with '/', it becomes the result outright.
 */
int path_join(const char *base, const char *rel, char *out) {
    if (!base || !rel || !out) return -EINVAL;

    if (rel[0] == '/') {
        /* Absolute rel — normalise and return directly */
        return path_normalize(rel, out);
    }

    /* Combine: base + "/" + rel, then normalise */
    char tmp[VFS_MOUNT_PATH_MAX * 2];
    ksnprintf(tmp, sizeof(tmp), "%s/%s", base, rel);
    return path_normalize(tmp, out);
}

/* ── path_dirname ────────────────────────────────────────────────────────── *
 * Copy the directory part of `path` into `out`.
 * e.g. "/foo/bar" → "/foo", "/" → "/".
 */
void path_dirname(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) return;
    size_t len = strlen(path);

    /* Trim trailing slashes */
    while (len > 1 && path[len-1] == '/') len--;

    /* Find last slash */
    size_t last = len;
    while (last > 0 && path[last-1] != '/') last--;
    if (last == 0) { strncpy(out, "/", out_sz); return; }
    if (last == 1) { strncpy(out, "/", out_sz); return; }

    size_t copy = last - 1; /* exclude trailing '/' */
    if (copy >= out_sz) copy = out_sz - 1;
    memcpy(out, path, copy);
    out[copy] = '\0';
}

/* ── path_basename ───────────────────────────────────────────────────────── *
 * Copy the final component of `path` into `out`.
 * e.g. "/foo/bar" → "bar", "/" → "/".
 */
void path_basename(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) return;
    size_t len = strlen(path);

    /* Trim trailing slashes (but keep at least one char) */
    while (len > 1 && path[len-1] == '/') len--;

    /* Find last '/' */
    size_t last = len;
    while (last > 0 && path[last-1] != '/') last--;

    size_t blen = len - last;
    if (blen == 0) { strncpy(out, "/", out_sz); return; }
    if (blen >= out_sz) blen = out_sz - 1;
    memcpy(out, path + last, blen);
    out[blen] = '\0';
}
