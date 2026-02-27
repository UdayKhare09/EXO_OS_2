sysdep_supported_options = {
    'posix': true,
    'linux': false,
    'glibc': false,
    'bsd': false,
}

exo_include_dirs = [
    include_directories('include'),
]

rtld_include_dirs += exo_include_dirs
libc_include_dirs += exo_include_dirs

rtld_sources += files(
    'generic.c',
    'syscall_stub.c',
    'generic_shim.cpp',
)

libc_sources += files(
    'generic.c',
    'syscall_stub.c',
    'generic_shim.cpp',
    'entry.cpp',
    host_machine.cpu_family() / 'spawn_thread.S',
)

if not no_headers
    install_subdir('include/abi-bits', install_dir: get_option('includedir'))
    install_headers('include/getopt.h')
    install_headers('include/mntent.h')
    install_headers('include/paths.h')
    install_headers('include/sys/ioctl.h', subdir: 'sys')
    install_headers('include/sys/sysmacros.h', subdir: 'sys')
endif

if headers_only
    subdir_done()
endif

if not headers_only
    custom_target('crt1',
        build_by_default: true,
        command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
        input: host_machine.cpu_family() / 'crt-src/crt1.S',
        output: 'crt1.o',
        install: true,
        install_dir: get_option('libdir')
    )

    custom_target('crti',
        build_by_default: true,
        command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
        input: host_machine.cpu_family() / 'crt-src/crti.S',
        output: 'crti.o',
        install: true,
        install_dir: get_option('libdir')
    )

    custom_target('crtn',
        build_by_default: true,
        command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
        input: host_machine.cpu_family() / 'crt-src/crtn.S',
        output: 'crtn.o',
        install: true,
        install_dir: get_option('libdir')
    )
endif
