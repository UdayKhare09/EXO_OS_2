//! path.rs — Path manipulation utilities (Rust rewrite of fs/path.c)
//!
//! Exposes the same four C-ABI symbols declared in `fs/vfs.h`:
//!
//! ```c
//! int  path_normalize(const char *in,  char *out);
//! int  path_join    (const char *base, const char *rel, char *out);
//! void path_dirname (const char *path, char *out, size_t out_sz);
//! void path_basename(const char *path, char *out, size_t out_sz);
//! ```
//!
//! Constants must match the C side (from `fs/vfs.h`):
//!   VFS_MOUNT_PATH_MAX = 512
//!   VFS_NAME_MAX       = 255
//!   EINVAL             = 22

use core::slice;

// ── Constants matching fs/vfs.h ────────────────────────────────────────────

const VFS_MOUNT_PATH_MAX: usize = 512;
const VFS_NAME_MAX: usize = 255;
const EINVAL: i32 = 22;

// ── Internal helpers ───────────────────────────────────────────────────────

/// Convert a C string pointer into a byte slice.
///
/// # Safety
/// `ptr` must be a valid, NUL-terminated C string with lifetime ≥ the slice.
unsafe fn cstr_to_bytes<'a>(ptr: *const u8) -> &'a [u8] {
    if ptr.is_null() {
        return &[];
    }
    let mut len = 0usize;
    // SAFETY: caller guarantees NUL-terminated string
    while unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    unsafe { slice::from_raw_parts(ptr, len) }
}

/// Write `src` bytes into `dst` starting at `dst[offset]`, then NUL-terminate.
/// Returns the new offset (offset + src.len()) or None if it would overflow.
fn buf_append(dst: &mut [u8], offset: usize, src: &[u8]) -> Option<usize> {
    let end = offset.checked_add(src.len())?;
    if end >= dst.len() {
        return None; // no room for src + NUL
    }
    dst[offset..end].copy_from_slice(src);
    dst[end] = 0;
    Some(end)
}

// ── path_normalize ─────────────────────────────────────────────────────────

/// Normalise an absolute path: collapse `//`, `.` and `..` components.
///
/// `out` must point to a buffer of at least `VFS_MOUNT_PATH_MAX` bytes.
/// Returns 0 on success, `-EINVAL` if `in` is not absolute or result overflows.
///
/// # Safety
/// `in_ptr` must be a valid NUL-terminated C string.
/// `out_ptr` must point to a writable buffer of at least `VFS_MOUNT_PATH_MAX` bytes.
#[no_mangle]
pub unsafe extern "C" fn path_normalize(in_ptr: *const u8, out_ptr: *mut u8) -> i32 {
    // Safety: callers pass valid C strings / buffers; we validate immediately.
    let input = unsafe { cstr_to_bytes(in_ptr) };
    if input.is_empty() || input[0] != b'/' {
        return -EINVAL;
    }
    if out_ptr.is_null() {
        return -EINVAL;
    }

    // SAFETY: caller guarantees VFS_MOUNT_PATH_MAX bytes of writable space.
    let out = unsafe { slice::from_raw_parts_mut(out_ptr, VFS_MOUNT_PATH_MAX) };

    // Start with "/"
    out[0] = b'/';
    out[1] = 0;
    let mut out_len: usize = 1; // bytes written (not including NUL)

    let mut i = 0usize;
    while i < input.len() {
        // Skip slashes
        while i < input.len() && input[i] == b'/' {
            i += 1;
        }
        if i >= input.len() {
            break;
        }

        // Find component end
        let comp_start = i;
        while i < input.len() && input[i] != b'/' {
            i += 1;
        }
        let comp = &input[comp_start..i];
        let clen = comp.len();

        // Reject overlong component
        if clen > VFS_NAME_MAX {
            return -EINVAL;
        }

        if clen == 1 && comp[0] == b'.' {
            // "." — skip
            continue;
        }

        if clen == 2 && comp[0] == b'.' && comp[1] == b'.' {
            // ".." — strip last component
            if out_len > 1 {
                // walk back past the last component
                while out_len > 1 && out[out_len - 1] != b'/' {
                    out_len -= 1;
                }
                // strip trailing slash (unless we're at root)
                if out_len > 1 {
                    out_len -= 1;
                }
                out[out_len] = 0;
            }
            continue;
        }

        // Append "/" (skip if we're still at root "/")
        if out_len > 1 {
            if out_len + 1 >= VFS_MOUNT_PATH_MAX {
                return -EINVAL;
            }
            out[out_len] = b'/';
            out_len += 1;
        }

        // Append component
        if out_len + clen >= VFS_MOUNT_PATH_MAX {
            return -EINVAL;
        }
        out[out_len..out_len + clen].copy_from_slice(comp);
        out_len += clen;
        out[out_len] = 0;
    }

    0
}

// ── path_join ─────────────────────────────────────────────────────────────

/// Join `base` (absolute) with `rel` (relative or absolute) into `out`.
///
/// If `rel` is absolute it is normalised and returned directly.
/// Otherwise `base/rel` is combined and normalised.
///
/// Returns 0 on success, `-EINVAL` on bad arguments or overflow.
///
/// # Safety
/// `base` and `rel` must be valid NUL-terminated C strings.
/// `out` must point to a writable buffer of at least `VFS_MOUNT_PATH_MAX` bytes.
#[no_mangle]
pub unsafe extern "C" fn path_join(
    base: *const u8,
    rel: *const u8,
    out: *mut u8,
) -> i32 {
    if base.is_null() || rel.is_null() || out.is_null() {
        return -EINVAL;
    }

    let rel_bytes = unsafe { cstr_to_bytes(rel) };

    if rel_bytes.first() == Some(&b'/') {
        // Absolute rel — normalise directly.
        return unsafe { path_normalize(rel, out) };
    }

    // Build "base/rel" into a temporary stack buffer, then normalise.
    // We use a fixed-size stack buffer (2 × VFS_MOUNT_PATH_MAX) matching
    // the C original, but build it safely without ksnprintf.
    let mut tmp = [0u8; VFS_MOUNT_PATH_MAX * 2];

    let base_bytes = unsafe { cstr_to_bytes(base) };

    let pos = 0usize;
    let Some(pos) = buf_append(&mut tmp, pos, base_bytes) else {
        return -EINVAL;
    };
    let Some(pos) = buf_append(&mut tmp, pos, b"/") else {
        return -EINVAL;
    };
    let Some(_pos) = buf_append(&mut tmp, pos, rel_bytes) else {
        return -EINVAL;
    };

    unsafe { path_normalize(tmp.as_ptr(), out) }
}

// ── path_dirname ──────────────────────────────────────────────────────────

/// Copy the directory component of `path` into `out[0..out_sz]`.
///
/// `"/foo/bar"` → `"/foo"`,  `"/foo"` → `"/"`,  `"/"` → `"/"`.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
/// `out` must point to a writable buffer of `out_sz` bytes.
#[no_mangle]
pub unsafe extern "C" fn path_dirname(path: *const u8, out: *mut u8, out_sz: usize) {
    if path.is_null() || out.is_null() || out_sz == 0 {
        return;
    }

    let bytes = unsafe { cstr_to_bytes(path) };
    let out_buf = unsafe { slice::from_raw_parts_mut(out, out_sz) };

    // Trim trailing slashes (keep at least one character)
    let mut len = bytes.len();
    while len > 1 && bytes[len - 1] == b'/' {
        len -= 1;
    }

    // Find last slash
    let mut last = len;
    while last > 0 && bytes[last - 1] != b'/' {
        last -= 1;
    }

    if last == 0 || last == 1 {
        // No slash found, or slash is at position 0 (i.e. root)
        let copy = out_sz.min(1);
        out_buf[..copy].copy_from_slice(&b"/"[..copy]);
        if copy < out_sz {
            out_buf[copy] = 0;
        }
        return;
    }

    // Copy up to last-1 characters (exclude the trailing slash)
    let copy = (last - 1).min(out_sz - 1);
    out_buf[..copy].copy_from_slice(&bytes[..copy]);
    out_buf[copy] = 0;
}

// ── path_basename ─────────────────────────────────────────────────────────

/// Copy the final component of `path` into `out[0..out_sz]`.
///
/// `"/foo/bar"` → `"bar"`,  `"/foo/"` → `"foo"`,  `"/"` → `"/"`.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
/// `out` must point to a writable buffer of `out_sz` bytes.
#[no_mangle]
pub unsafe extern "C" fn path_basename(path: *const u8, out: *mut u8, out_sz: usize) {
    if path.is_null() || out.is_null() || out_sz == 0 {
        return;
    }

    let bytes = unsafe { cstr_to_bytes(path) };
    let out_buf = unsafe { slice::from_raw_parts_mut(out, out_sz) };

    // Trim trailing slashes (keep at least one)
    let mut len = bytes.len();
    while len > 1 && bytes[len - 1] == b'/' {
        len -= 1;
    }

    // Find last slash
    let mut last = len;
    while last > 0 && bytes[last - 1] != b'/' {
        last -= 1;
    }

    let blen = len - last;
    if blen == 0 {
        let copy = out_sz.min(1);
        out_buf[..copy].copy_from_slice(&b"/"[..copy]);
        if copy < out_sz {
            out_buf[copy] = 0;
        }
        return;
    }

    let copy = blen.min(out_sz - 1);
    out_buf[..copy].copy_from_slice(&bytes[last..last + copy]);
    out_buf[copy] = 0;
}
