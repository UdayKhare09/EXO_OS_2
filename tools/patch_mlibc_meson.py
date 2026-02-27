#!/usr/bin/env python3
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_mlibc_meson.py <mlibc_repo_dir>", file=sys.stderr)
        return 2

    repo = Path(sys.argv[1])
    meson_path = repo / "meson.build"
    if not meson_path.exists():
        print(f"meson.build not found: {meson_path}", file=sys.stderr)
        return 1

    text = meson_path.read_text()
    marker = "elif host_machine.system() == 'exo'"
    if marker in text:
        return 0

    insert = (
        "elif host_machine.system() == 'exo'\n"
        "\trtld_include_dirs += include_directories('sysdeps/exo/include')\n"
        "\tlibc_include_dirs += include_directories('sysdeps/exo/include')\n"
        "\tinternal_conf.set10('MLIBC_MAP_DSO_SEGMENTS', true)\n"
        "\tinternal_conf.set10('MLIBC_MMAP_ALLOCATE_DSO', true)\n"
        "\tinternal_conf.set10('MLIBC_MAP_FILE_WINDOWS', true)\n"
        "\tsubdir('sysdeps/exo')\n"
    )

    anchor = "# ANCHOR: demo-sysdeps"
    if anchor in text:
        text = text.replace(anchor, insert + anchor, 1)
    else:
        needle = "else\n\terror('No sysdeps defined for OS: ' + host_machine.system())\nendif"
        if needle not in text:
            print("Could not find insertion point in mlibc meson.build", file=sys.stderr)
            return 1
        text = text.replace(needle, insert + needle, 1)

    meson_path.write_text(text)
    print(">>> Patched mlibc meson.build for exo sysdeps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
