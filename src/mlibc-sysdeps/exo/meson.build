# meson.build — mlibc sysdep layer for EXO_OS
#
# To use: symlink or copy this directory into mlibc/sysdeps/exo/
# Then build mlibc with:
#   meson setup build --cross-file exo-cross.ini -Dlinux_kernel_headers=disabled
#
# This file tells mlibc which sysdeps sources to compile and which
# mlibc subsystems our OS supports.

# The rtdl (runtime dynamic linker) is not needed for static linking.
# If you want dynamic linking later, add rtdl sources here.

rtdl_sources = []

libc_sources = files(
    'entry.c',
    'generic.c',
)

# Tell mlibc which optional subsystems we support
# (mlibc checks these variables to enable POSIX, Linux, etc.)
# For a minimal port, we need at minimum:
#   - sys_read/write/open/close
#   - sys_vm_map/vm_unmap (mmap)
#   - sys_exit
#   - sys_tcb_set (TLS)
#   - sys_anon_allocate/anon_free (for internal allocator)
#   - sys_clock_gettime

# Headers our sysdep provides
# install_headers('exo/abi-bits/abi.h', subdir: 'abi-bits')

# Disable Linux-specific kernel headers requirement
# (we provide our own definitions)
