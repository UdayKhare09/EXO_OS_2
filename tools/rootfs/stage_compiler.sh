#!/bin/sh
# tools/rootfs/stage_compiler.sh
#
# Stages host GCC (driver, internals, CRT objects, static libs) and all
# header trees into rootfs staging dirs, then generates a single debugfs
# batch commands file used by the ROOT_IMG make recipe.
#
# Required env vars:
#   GCC_VERSION        — e.g. 15.2.1        (gcc -dumpversion)
#   GCC_TRIPLET        — e.g. x86_64-pc-linux-gnu  (gcc -dumpmachine)
#   GCC_LIBDIR         — e.g. /usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1
#   COMPILER_BIN_DIR   — staging: user-facing binaries (gcc, g++, as, ld, …)
#   COMPILER_INT_DIR   — staging: GCC internals (cc1, collect2, crt*.o, …)
#   COMPILER_INC_DIR   — staging: header trees
#   COMPILER_DBG_CMDS  — output: debugfs batch commands file for ROOT_IMG
#   SRC_KERNEL_DIR     — path to src/kernel/ for EXO_OS headers

set -e

if [ -z "${GCC_VERSION}" ] || [ -z "${GCC_TRIPLET}" ] || [ -z "${GCC_LIBDIR}" ]; then
    echo "!!! ERROR: GCC not found on host. Install gcc and retry." >&2
    exit 1
fi

echo ">>> Staging GCC ${GCC_VERSION} (${GCC_TRIPLET}) into rootfs..."

mkdir -p \
    "${COMPILER_BIN_DIR}" \
    "${COMPILER_INT_DIR}" \
    "${COMPILER_INC_DIR}/gcc-include" \
    "${COMPILER_INC_DIR}/sys-include" \
    "${COMPILER_INC_DIR}/exo-include"

# Runtime linker extras (CRT objects + linker stubs) go to a separate staging
# dir so the debugfs emit can write them to /usr/lib/ (not the GCC libdir).
RUNTIME_EXT_DIR="$(dirname "${COMPILER_BIN_DIR}")/runtime-ext"
mkdir -p "${RUNTIME_EXT_DIR}"

# ── 1. User-facing compiler + binutils binaries ──────────────────────────────
echo "  [1/6] Staging compiler driver + binutils binaries..."
for b in gcc cc ar nm strip objcopy objdump ranlib addr2line; do
    p=$(which "${b}" 2>/dev/null) || continue
    cp "${p}" "${COMPILER_BIN_DIR}/${b}"
    chmod +x "${COMPILER_BIN_DIR}/${b}"
done

# Assembler — try triplet-prefixed first, then plain
for b in "${GCC_TRIPLET}-as" as; do
    p=$(which "${b}" 2>/dev/null) || continue
    cp "${p}" "${COMPILER_BIN_DIR}/as"
    chmod +x "${COMPILER_BIN_DIR}/as"
    break
done

# Linker — prefer ld.bfd for native linking
for b in ld.bfd ld; do
    p=$(which "${b}" 2>/dev/null) || continue
    cp "${p}" "${COMPILER_BIN_DIR}/ld"
    cp "${p}" "${COMPILER_BIN_DIR}/ld.bfd"
    chmod +x "${COMPILER_BIN_DIR}/ld"
    chmod +x "${COMPILER_BIN_DIR}/ld.bfd"
    break
done

# Ensure cc → gcc symlink (copy)
[ ! -f "${COMPILER_BIN_DIR}/cc" ] && [ -f "${COMPILER_BIN_DIR}/gcc" ] && \
    cp "${COMPILER_BIN_DIR}/gcc" "${COMPILER_BIN_DIR}/cc"

# ── 2. GCC internals (cc1, collect2, lto1, lto-wrapper) ─────────────────────
echo "  [2/6] Staging GCC internals..."
for b in cc1 collect2 lto1 lto-wrapper; do
    f="${GCC_LIBDIR}/${b}"
    [ -f "${f}" ] && cp "${f}" "${COMPILER_INT_DIR}/${b}" && chmod +x "${COMPILER_INT_DIR}/${b}"
done
[ -f "${GCC_LIBDIR}/liblto_plugin.so" ] && \
    cp "${GCC_LIBDIR}/liblto_plugin.so" "${COMPILER_INT_DIR}/"

# ── 3. CRT objects + static libs ─────────────────────────────────────────────
echo "  [3/6] Staging CRT objects + static libs..."
for f in \
    crtbegin.o crtbeginS.o crtbeginT.o \
    crtend.o crtendS.o crtfastmath.o \
    crtprec32.o crtprec64.o crtprec80.o \
    libgcc.a libgcc_eh.a libgcov.a; do
    src="${GCC_LIBDIR}/${f}"
    [ -f "${src}" ] && cp "${src}" "${COMPILER_INT_DIR}/${f}"
done

# 3b. Glibc CRT objects + linker stubs → /usr/lib/ in the guest.
# GCC calls ld with bare filenames like Scrt1.o / crti.o / crtn.o which ld
# resolves via its default library search path (/usr/lib).  The .so files here
# are GNU ld linker scripts (not ELFs) that point to the actual .so.N libraries
# already staged by the main runtime-lib staging loop.
echo "  [3b/6] Staging glibc CRT objects + linker stubs (→ /usr/lib/)..."
for f in Scrt1.o crt1.o crti.o crtn.o; do
    [ -f "/usr/lib/${f}" ] && cp "/usr/lib/${f}" "${RUNTIME_EXT_DIR}/${f}"
done
for f in libc.so libm.so libgcc_s.so; do
    [ -f "/usr/lib/${f}" ] && cp "/usr/lib/${f}" "${RUNTIME_EXT_DIR}/${f}"
done
[ -f /usr/lib/libc_nonshared.a ] && cp /usr/lib/libc_nonshared.a "${RUNTIME_EXT_DIR}/libc_nonshared.a"

# ── 4. GCC internal includes ──────────────────────────────────────────────────
echo "  [4/6] Staging GCC internal headers..."
[ -d "${GCC_LIBDIR}/include" ] && \
    cp -a "${GCC_LIBDIR}/include/." "${COMPILER_INC_DIR}/gcc-include/"
[ -d "${GCC_LIBDIR}/include-fixed" ] && \
    cp -a "${GCC_LIBDIR}/include-fixed/." "${COMPILER_INC_DIR}/gcc-include/"

# ── 5. System C headers — strict allowlist (no wine, winpr, linux uAPI…) ────
echo "  [5/6] Staging core C / POSIX headers (allowlist only)..."
# Only copy top-level .h files and well-known POSIX/glibc subdirectories.
# Everything else (wine, winpr3, linux, asm*, drm, sound, …) is excluded.
CORE_SUBDIRS="sys bits gnu arpa net netinet netpacket protocols rpc"

# Top-level .h files
find /usr/include -maxdepth 1 -name '*.h' -print0 | \
    xargs -0 -I{} cp {} "${COMPILER_INC_DIR}/sys-include/"

# Approved subdirectories
for d in ${CORE_SUBDIRS}; do
    [ -d "/usr/include/${d}" ] && cp -a "/usr/include/${d}" "${COMPILER_INC_DIR}/sys-include/${d}"
done

# ── 6. EXO_OS kernel headers ─────────────────────────────────────────────────
echo "  [6/6] Staging EXO_OS kernel headers..."
find "${SRC_KERNEL_DIR}" -name '*.h' | while read -r h; do
    rel="${h#${SRC_KERNEL_DIR}/}"
    dir="${COMPILER_INC_DIR}/exo-include/$(dirname "${rel}")"
    mkdir -p "${dir}"
    cp "${h}" "${dir}/"
done

# ── Generate debugfs batch commands file ──────────────────────────────────────
# The ROOT_IMG recipe calls: debugfs -w -f $COMPILER_DBG_CMDS $ROOT_IMG
# All mkdir + write commands are batched in a single debugfs session for speed.
echo ">>> Generating debugfs batch commands file: ${COMPILER_DBG_CMDS}"
> "${COMPILER_DBG_CMDS}"

GUEST_GCC_BASE="usr/lib/gcc/${GCC_TRIPLET}/${GCC_VERSION}"
GUEST_GCC_INC="${GUEST_GCC_BASE}/include"
GUEST_SINC="usr/include"
GUEST_EINC="usr/include/exo"

# emit_mkdirs <guest/path>
# Appends one "mkdir" line per path segment to $COMPILER_DBG_CMDS.
# Duplicate mkdirs silently fail in debugfs — that is fine.
emit_mkdirs() {
    local acc="" seg
    echo "$1" | tr '/' '\n' | while read -r seg; do
        [ -z "${seg}" ] && continue
        if [ -z "${acc}" ]; then acc="${seg}"; else acc="${acc}/${seg}"; fi
        echo "mkdir ${acc}"
    done >> "${COMPILER_DBG_CMDS}"
}

# Pre-create top-level dirs that may not exist in the static $(foreach) list
emit_mkdirs "${GUEST_GCC_BASE}"
emit_mkdirs "${GUEST_GCC_INC}"
emit_mkdirs "${GUEST_SINC}"
emit_mkdirs "${GUEST_EINC}"

# Compiler user binaries → usr/bin/
for f in "${COMPILER_BIN_DIR}"/*; do
    [ -f "${f}" ] || continue
    n=$(basename "${f}")
    echo "write ${f} usr/bin/${n}" >> "${COMPILER_DBG_CMDS}"
done

# GCC internals → usr/lib/gcc/TRIPLET/VERSION/
for f in "${COMPILER_INT_DIR}"/*; do
    [ -f "${f}" ] || continue
    n=$(basename "${f}")
    echo "write ${f} ${GUEST_GCC_BASE}/${n}" >> "${COMPILER_DBG_CMDS}"
done

# Glibc CRT objects + linker stubs → usr/lib/
for f in "${RUNTIME_EXT_DIR}"/*; do
    [ -f "${f}" ] || continue
    n=$(basename "${f}")
    echo "write ${f} usr/lib/${n}" >> "${COMPILER_DBG_CMDS}"
done

# GCC internal headers → usr/lib/gcc/TRIPLET/VERSION/include/
# sort guarantees parent dirs appear before their children
find "${COMPILER_INC_DIR}/gcc-include" -mindepth 1 | sort | while read -r f; do
    rel="${f#${COMPILER_INC_DIR}/gcc-include/}"
    if [ -d "${f}" ]; then
        echo "mkdir ${GUEST_GCC_INC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    else
        echo "write ${f} ${GUEST_GCC_INC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    fi
done

# System C headers → usr/include/
find "${COMPILER_INC_DIR}/sys-include" -mindepth 1 | sort | while read -r f; do
    rel="${f#${COMPILER_INC_DIR}/sys-include/}"
    if [ -d "${f}" ]; then
        echo "mkdir ${GUEST_SINC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    else
        echo "write ${f} ${GUEST_SINC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    fi
done

# EXO_OS kernel headers → usr/include/exo/
find "${COMPILER_INC_DIR}/exo-include" -mindepth 1 | sort | while read -r f; do
    rel="${f#${COMPILER_INC_DIR}/exo-include/}"
    if [ -d "${f}" ]; then
        echo "mkdir ${GUEST_EINC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    else
        echo "write ${f} ${GUEST_EINC}/${rel}" >> "${COMPILER_DBG_CMDS}"
    fi
done

n=$(wc -l < "${COMPILER_DBG_CMDS}")
echo ">>> Compiler staging complete: ${n} debugfs commands prepared."
touch "${COMPILER_BIN_DIR}/.stamp"
