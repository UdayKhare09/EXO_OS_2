# EXO_OS Root Makefile
# Limine 8 (UEFI) bootloader — GPT disk image with three partitions:
#   p1: EFI System (FAT32, 64 MiB)  → /boot
#   p2: Linux Root (ext2)            → /
#   p3: Linux Data (ext2)            → /mnt

MAKEFLAGS += --no-builtin-rules

# ── Paths ─────────────────────────────────────────────────────────────────────
ARCH      := x86_64
BUILD_DIR := build
LIMINE_DIR := $(BUILD_DIR)/limine

DISK_IMG   := $(BUILD_DIR)/exo.img
KERNEL_ELF := $(BUILD_DIR)/exo.elf

comma := ,

# ── Limine ────────────────────────────────────────────────────────────────────
LIMINE_TAG  := v8.x-binary
LIMINE_REPO := https://github.com/limine-bootloader/limine.git

# ── Disk layout (512-byte sectors) ───────────────────────────────────────────
DISK_SIZE_MB := 8192
# p1: EFI System — 64 MiB; guarantees ≥65525 FAT32 clusters (UEFI spec)
EFI_START    := 2048
EFI_END      := 133119
EFI_SECTORS  := $(shell echo $$(($(EFI_END) - $(EFI_START) + 1)))
# p2: Linux root — fixed-size partition
ROOT_START    := 133120
ROOT_SIZE_MB  := 6144
ROOT_SECTORS  := $(shell echo $$(($(ROOT_SIZE_MB) * 2048)))
ROOT_END      := $(shell echo $$(($(ROOT_START) + $(ROOT_SECTORS) - 1)))
# p3: Linux data — remainder up to backup GPT
DATA_START    := $(shell echo $$(($(ROOT_END) + 1)))
DATA_END      := $(shell echo $$((($(DISK_SIZE_MB) * 2048) - 34)))
DATA_SECTORS  := $(shell echo $$(($(DATA_END) - $(DATA_START) + 1)))

EFI_IMG  := $(BUILD_DIR)/efi.img
ROOT_IMG := $(BUILD_DIR)/root.img
DATA_IMG := $(BUILD_DIR)/data.img

# ── OVMF firmware ─────────────────────────────────────────────────────────────
# Auto-detect OVMF firmware: Fedora/Arch path first, then Debian/Ubuntu.
OVMF_CODE      ?= $(or $(wildcard /usr/share/edk2/x64/OVMF_CODE.4m.fd),\
                       $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd),\
                       $(wildcard /usr/share/OVMF/OVMF_CODE.fd),\
                       /usr/share/edk2/x64/OVMF_CODE.4m.fd)
OVMF_VARS_TMPL := /usr/share/edk2/x64/OVMF_VARS.4m.fd
OVMF_VARS      := $(BUILD_DIR)/OVMF_VARS.4m.fd

# ── Toolchain ─────────────────────────────────────────────────────────────────
CC      := clang
LD      := ld.lld
AS      := clang
NASM    := nasm
OBJCOPY := llvm-objcopy

TARGET_TRIPLE := x86_64-unknown-elf

# ── Rust addon ────────────────────────────────────────────────────────────────
# src/rust/ is a no_std staticlib that adds Rust components to the kernel.
# It is compiled for x86_64-unknown-none (bare-metal, kernel code model).
RUST_CRATE_DIR  := src/rust
RUST_TARGET_DIR := $(BUILD_DIR)/rust-target
RUST_PROFILE    := release
RUST_TARGET     := x86_64-unknown-none
RUST_LIB        := $(RUST_TARGET_DIR)/$(RUST_TARGET)/$(RUST_PROFILE)/libexo_rust.a
SRC_DIR  := src/kernel
ARCH_DIR := $(SRC_DIR)/arch/x86_64

CFLAGS := \
    --target=$(TARGET_TRIPLE) \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -m64 -march=x86-64 \
    -msse -msse2 -msse3 -mssse3 -msse4.1 -msse4.2 \
    -mavx -mavx2 -mf16c -mfma \
    -maes -mpclmul -mpopcnt \
    -mbmi -mbmi2 -mlzcnt \
    -mfpmath=sse \
    -mno-red-zone \
    -mcmodel=kernel \
    -O2 -g \
    -Wall -Wextra -Wno-unused-parameter \
    -I$(SRC_DIR) -I$(BUILD_DIR) \
    -MMD -MP

ASFLAGS   := $(CFLAGS)
NASMFLAGS := -f elf64 -g

LDFLAGS := \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    -T $(SRC_DIR)/linker.ld

# ── Sources & objects ─────────────────────────────────────────────────────────
# Path helpers are now implemented in src/rust/src/path.rs and exported from
# libexo_rust.a, so the legacy C implementation is intentionally deleted and not globbed here.
C_SRCS    := $(shell find $(SRC_DIR) -name '*.c')
AS_SRCS   := $(shell find $(SRC_DIR) -name '*.S')
NASM_SRCS := $(filter-out $(ARCH_DIR)/trampoline.asm, \
                 $(shell find $(SRC_DIR) -name '*.asm'))

C_OBJS    := $(patsubst $(SRC_DIR)/%.c,   $(BUILD_DIR)/obj/%.c.o,   $(C_SRCS))
AS_OBJS   := $(patsubst $(SRC_DIR)/%.S,   $(BUILD_DIR)/obj/%.S.o,   $(AS_SRCS))
NASM_OBJS := $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/obj/%.asm.o, $(NASM_SRCS))

TRAMPOLINE_SRC := $(ARCH_DIR)/trampoline.asm
TRAMPOLINE_BIN := $(BUILD_DIR)/obj/arch/x86_64/trampoline.bin
TRAMPOLINE_OBJ := $(BUILD_DIR)/obj/arch/x86_64/trampoline.o

# ── Font pipeline (host tool: TrueType → C array) ────────────────────────────
FONT_SIZE     ?= 20
FONT_TOOL     := $(BUILD_DIR)/tools/gen_font
FONT_TTF      := $(SRC_DIR)/gfx/3rdparty/font.ttf
FONT_DATA_C   := $(BUILD_DIR)/gfx_font_data.c
FONT_DATA_OBJ := $(BUILD_DIR)/obj/gfx_font_data.c.o

ALL_OBJS := $(C_OBJS) $(AS_OBJS) $(NASM_OBJS) $(TRAMPOLINE_OBJ) $(FONT_DATA_OBJ) $(RUST_LIB)
DEPS     := $(C_OBJS:.o=.d)
-include $(DEPS)

# ── Production userspace staging ─────────────────────────────────────────────
USER_CC      ?= gcc
USER_LINKER  ?= /lib64/ld-linux-x86-64.so.2
GLIBC_LIBC   ?= $(shell gcc -print-file-name=libc.so.6 2>/dev/null)
GLIBC_LIBM   ?= $(shell gcc -print-file-name=libm.so.6 2>/dev/null)
GLIBC_LIBDL  ?= $(shell ldconfig -p 2>/dev/null | awk '/libdl\.so\.2 .*x86-64/{print $$NF; exit}')
GLIBC_LIBPTHREAD ?= $(shell ldconfig -p 2>/dev/null | awk '/libpthread\.so\.0 .*x86-64/{print $$NF; exit}')
GLIBC_LD_SO      ?= $(shell ldconfig -p 2>/dev/null | awk '/ld-linux-x86-64\.so\.2/{print $$NF; exit}')
GLIBC_LIBCRYPT   ?= $(shell ldconfig -p 2>/dev/null | awk '/libcrypt\.so\.2 .*x86-64/{print $$NF; exit}; /libcrypt\.so\.1 .*x86-64/{print $$NF; exit}')

ROOTFS_STAGE_DIR     := $(BUILD_DIR)/rootfs-prod
RUNTIME_LIB_DIR      := $(ROOTFS_STAGE_DIR)/runtime-lib
RUNTIME_LIB_STAMP    := $(RUNTIME_LIB_DIR)/.stamp
RUNTIME_DEPS_STAMP   := $(ROOTFS_STAGE_DIR)/.deps-stamp

# ── GCC compiler (host copy staged into rootfs) ──────────────────────────────
GCC_VERSION  := $(shell gcc -dumpversion 2>/dev/null)
GCC_TRIPLET  := $(shell gcc -dumpmachine 2>/dev/null)
GCC_LIBDIR   := $(shell dirname $$(gcc -print-libgcc-file-name 2>/dev/null))

COMPILER_BIN_DIR  := $(ROOTFS_STAGE_DIR)/compiler-bin
COMPILER_INT_DIR  := $(ROOTFS_STAGE_DIR)/compiler-int
COMPILER_INC_DIR  := $(ROOTFS_STAGE_DIR)/compiler-inc
COMPILER_DBG_CMDS := $(ROOTFS_STAGE_DIR)/compiler-debugfs-cmds.txt
COMPILER_STAMP    := $(COMPILER_BIN_DIR)/.stamp

# ── Coreutils + shell from host ──────────────────────────────────────────────
HOST_SHELL   ?= $(shell which bash 2>/dev/null || which dash 2>/dev/null)
HOST_CORE_BINS := ls cat echo cp mv rm mkdir chmod ln pwd env id whoami \
                  grep sed awk sort head tail wc cut tr date uname find xargs \
                  true false test stat touch sleep kill ps clear which poweroff reboot \
                  fish ssh ping dig vi vim nano less curl git wget tar unzip zip
COREUTILS_DIR   := $(ROOTFS_STAGE_DIR)/coreutils
COREUTILS_STAMP := $(COREUTILS_DIR)/.stamp

# ── External binaries (configured in script) ─────────────────────────────────
EXTERNAL_BIN_LIST_SH := tools/rootfs/external_bins.sh
EXTERNAL_BIN_DIR     := $(ROOTFS_STAGE_DIR)/external-bin
EXTERNAL_BIN_STAMP   := $(EXTERNAL_BIN_DIR)/.stamp

USERSPACE_STAGE_INPUTS = $(COREUTILS_STAMP) $(EXTERNAL_BIN_STAMP) $(INIT_BIN) $(SU_BIN) $(COMPILER_STAMP)
ROOTFS_DIRS := \
	dev tmp boot proc sys sys/bus sys/bus/usb sys/bus/usb/devices sys/bus/pci \
	sys/bus/pci/devices home home/root home/uday \
	etc etc/ssl etc/ssl/certs \
	bin lib lib/x86_64-linux-gnu lib64 \
	usr usr/bin usr/sbin usr/lib usr/lib/x86_64-linux-gnu \
	usr/include usr/share usr/share/terminfo usr/share/terminfo/l \
	sbin var run mnt dev/shm

# ── rootfs config files ──────────────────────────────────────────────
ROOTFS_NSSWITCH := tools/rootfs/nsswitch.conf
ROOTFS_LD_CONF  := tools/rootfs/ld.so.conf
ROOTFS_PROFILE  := tools/rootfs/profile
ROOTFS_ENV_FILE := tools/rootfs/environment
ROOTFS_PASSWD   := tools/rootfs/passwd
ROOTFS_SHADOW   := tools/rootfs/shadow
ROOTFS_GROUP    := tools/rootfs/group
ROOTFS_RESOLV   := tools/rootfs/resolv.conf
ROOTFS_MAIN_C   := tools/rootfs/main.c
ROOTFS_TERMINFO_LINUX := $(shell sh -c 'for p in /usr/share/terminfo/l/linux /lib/terminfo/l/linux /etc/terminfo/l/linux; do [ -f "$$p" ] && { echo "$$p"; exit 0; }; done')
ROOTFS_CA_BUNDLE := $(shell sh -c 'for p in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do [ -f "$$p" ] && { echo "$$p"; exit 0; }; done')

# ── QEMU flags (shared between run and run-debug) ─────────────────────────────
QEMU_MEMORY   ?= 6G
QEMU_SMP      ?= 6
QEMU_FLAGS := \
    -machine q35 \
    -cpu host \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(OVMF_VARS) \
    -drive id=hd0,file=$(DISK_IMG),format=raw,if=none \
    -device virtio-blk-pci,drive=hd0 \
    -m $(QEMU_MEMORY) \
    -smp $(QEMU_SMP) \
    -serial stdio \
    -vga std \
    -device nec-usb-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -no-reboot

.PHONY: all clean distclean run run-debug

all: $(DISK_IMG)

# ── Limine ────────────────────────────────────────────────────────────────────
$(LIMINE_DIR)/limine.h:
	@echo ">>> Cloning Limine 8..."
	@mkdir -p $(LIMINE_DIR)
	git clone --branch $(LIMINE_TAG) --depth=1 $(LIMINE_REPO) $(LIMINE_DIR)
	rm -rf $(LIMINE_DIR)/.git
	@echo ">>> Building limine install tool..."
	cc -o $(LIMINE_DIR)/limine $(LIMINE_DIR)/limine.c

$(BUILD_DIR)/limine.h: $(LIMINE_DIR)/limine.h
	cp $< $@

# ── Font ──────────────────────────────────────────────────────────────────────
$(FONT_TOOL): tools/gen_font.c
	@mkdir -p $(dir $@)
	cc -O2 -o $@ $< -lm

$(FONT_DATA_C): $(FONT_TOOL) $(FONT_TTF)
	@mkdir -p $(dir $@)
	$(FONT_TOOL) $(FONT_TTF) $(FONT_SIZE) $@

$(FONT_DATA_OBJ): $(FONT_DATA_C) $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Trampoline: flat binary wrapped in an ELF object ─────────────────────────
$(TRAMPOLINE_BIN): $(TRAMPOLINE_SRC)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -o $@ $<

$(TRAMPOLINE_OBJ): $(TRAMPOLINE_BIN)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

# ── Kernel ────────────────────────────────────────────────────────────────────
$(BUILD_DIR)/obj/%.c.o: $(SRC_DIR)/%.c $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/%.S.o: $(SRC_DIR)/%.S $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/%.asm.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ── Rust staticlib ───────────────────────────────────────────────────────────
# cargo --manifest-path keeps the invocation hermetic: no need to cd.
# CARGO_TARGET_DIR is set explicitly so the build artefacts land inside
# build/ rather than src/rust/target/ (which would confuse git clean).
$(RUST_LIB): $(shell find $(RUST_CRATE_DIR)/src -name '*.rs') $(RUST_CRATE_DIR)/Cargo.toml
	@echo ">>> Building Rust addon ($(RUST_PROFILE))..."
	@mkdir -p $(RUST_TARGET_DIR)
	CARGO_TARGET_DIR=$(abspath $(RUST_TARGET_DIR)) \
	    cargo build --$(RUST_PROFILE) \
	        --manifest-path $(RUST_CRATE_DIR)/Cargo.toml \
	        --target $(RUST_TARGET) \
	        -q
	@echo ">>> Rust addon built: $@"

$(KERNEL_ELF): $(ALL_OBJS)
	@echo ">>> Linking kernel..."
	$(LD) $(LDFLAGS) $^ -o $@

# ── Runtime libraries and setup ─────────────────────────────────────────────
$(RUNTIME_LIB_STAMP):
	@test -f "$(GLIBC_LIBC)" || (echo "Missing glibc: run 'apt install gcc' or set GLIBC_LIBC"; exit 1)
	@test -f "$(GLIBC_LD_SO)" || (echo "Missing dynamic linker: $(GLIBC_LD_SO)"; exit 1)
	@test -f "$(GLIBC_LIBCRYPT)" || (echo "Missing libcrypt: $(GLIBC_LIBCRYPT)"; exit 1)
	@mkdir -p $(RUNTIME_LIB_DIR)
	cp $(GLIBC_LIBC)      $(RUNTIME_LIB_DIR)/libc.so.6
	cp $(GLIBC_LIBM)      $(RUNTIME_LIB_DIR)/libm.so.6
	@if [ -n "$(GLIBC_LIBDL)" ] && [ -f "$(GLIBC_LIBDL)" ]; then \
		cp $(GLIBC_LIBDL) $(RUNTIME_LIB_DIR)/libdl.so.2; \
	else \
		cp $(GLIBC_LIBC)  $(RUNTIME_LIB_DIR)/libdl.so.2; \
	fi
	@if [ -n "$(GLIBC_LIBPTHREAD)" ] && [ -f "$(GLIBC_LIBPTHREAD)" ]; then \
		cp $(GLIBC_LIBPTHREAD) $(RUNTIME_LIB_DIR)/libpthread.so.0; \
	else \
		cp $(GLIBC_LIBC)       $(RUNTIME_LIB_DIR)/libpthread.so.0; \
	fi
	cp $(GLIBC_LD_SO)     $(RUNTIME_LIB_DIR)/ld-linux-x86-64.so.2
	cp $(GLIBC_LIBCRYPT)  $(RUNTIME_LIB_DIR)/libcrypt.so.2
	@touch $@

$(COREUTILS_STAMP):
	@HOST_CORE_BINS="$(HOST_CORE_BINS)" HOST_SHELL="$(HOST_SHELL)" \
		COREUTILS_DIR="$(COREUTILS_DIR)" sh tools/rootfs/stage_bins.sh

$(COMPILER_STAMP): $(RUNTIME_LIB_STAMP) tools/rootfs/stage_compiler.sh
	@GCC_VERSION="$(GCC_VERSION)" GCC_TRIPLET="$(GCC_TRIPLET)" \
		GCC_LIBDIR="$(GCC_LIBDIR)" \
		COMPILER_BIN_DIR="$(COMPILER_BIN_DIR)" \
		COMPILER_INT_DIR="$(COMPILER_INT_DIR)" \
		COMPILER_INC_DIR="$(COMPILER_INC_DIR)" \
		COMPILER_DBG_CMDS="$(COMPILER_DBG_CMDS)" \
		SRC_KERNEL_DIR="$(SRC_DIR)" \
		sh tools/rootfs/stage_compiler.sh

$(EXTERNAL_BIN_STAMP): $(EXTERNAL_BIN_LIST_SH)
	@echo ">>> Fetching external binaries from $(EXTERNAL_BIN_LIST_SH)..."
	@mkdir -p $(EXTERNAL_BIN_DIR)
	@set -e; \
	. ./$(EXTERNAL_BIN_LIST_SH); \
	external_bins | while read -r name url; do \
		if [ -z "$$name" ] || [ -z "$$url" ]; then continue; fi; \
		echo "    - $$name <= $$url"; \
		curl -L --fail -o "$(EXTERNAL_BIN_DIR)/$$name" "$$url"; \
		chmod +x "$(EXTERNAL_BIN_DIR)/$$name"; \
	done
	@touch $@

# ── init / su (glibc static — installed as /sbin/init and /bin/su in rootfs) ──
INIT_SRC := tools/init.c
INIT_BIN := $(BUILD_DIR)/init
SU_SRC   := tools/su.c
SU_BIN   := $(BUILD_DIR)/su

$(INIT_BIN): $(INIT_SRC)
	@mkdir -p $(dir $@)
	$(USER_CC) -O2 -o $@ $< -lcrypt -Wl,-dynamic-linker,$(USER_LINKER)

$(SU_BIN): $(SU_SRC)
	@mkdir -p $(dir $@)
	$(USER_CC) -O2 -o $@ $< -lcrypt -Wl,-dynamic-linker,$(USER_LINKER)

# ── EFI partition (FAT32, no root needed — mtools) ────────────────────────────
$(EFI_IMG): $(KERNEL_ELF) $(LIMINE_DIR)/limine.h
	@echo ">>> Building EFI partition (FAT32, $(EFI_SECTORS) sectors)..."
	dd if=/dev/zero of=$@ bs=512 count=$(EFI_SECTORS) 2>/dev/null
	mkfs.fat -F 32 -n "EFI" $@ >/dev/null
	mmd   -i $@ ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@ $(LIMINE_DIR)/BOOTX64.EFI  ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL_ELF)              ::/boot/exo.elf
	mcopy -i $@ src/boot/limine.conf       ::/boot/limine/limine.conf

# ── Root partition (ext2, no root needed — debugfs) ───────────────────────────
# Auto-discover all transitive .so deps of every staged binary and copy to the
# production runtime-lib staging directory.
$(RUNTIME_DEPS_STAMP): $(RUNTIME_LIB_STAMP) $(USERSPACE_STAGE_INPUTS)
	@echo ">>> Auto-discovering shared library dependencies..."
	@bins_file="$(ROOTFS_STAGE_DIR)/runtime-deps.txt"; \
	 find $(COREUTILS_DIR) $(EXTERNAL_BIN_DIR) $(COMPILER_BIN_DIR) $(COMPILER_INT_DIR) \
	     -maxdepth 1 -type f 2>/dev/null > "$$bins_file"; \
	 for b in $(INIT_BIN) $(SU_BIN); do [ -f "$$b" ] && echo "$$b"; done >> "$$bins_file"; \
	 xargs -r ldd < "$$bins_file" 2>/dev/null \
	 | awk '/=> \//{print $$3}' | sort -u \
	 | while read -r so; do \
		 [ -f "$$so" ] || continue; \
		 name=$$(basename "$$so"); \
		 dest="$(RUNTIME_LIB_DIR)/$$name"; \
		 [ -f "$$dest" ] || { echo "  staging $$name"; cp "$$so" "$$dest"; }; \
	   done
	@touch $@

$(ROOT_IMG): $(RUNTIME_DEPS_STAMP) $(USERSPACE_STAGE_INPUTS) \
             $(ROOTFS_PROFILE) $(ROOTFS_ENV_FILE) $(ROOTFS_PASSWD) $(ROOTFS_SHADOW) \
             $(ROOTFS_GROUP) $(ROOTFS_RESOLV) $(ROOTFS_NSSWITCH) $(ROOTFS_LD_CONF)
	@echo ">>> Building root partition (ext2)..."
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_ROOT" -b 4096 $@
	$(foreach d,$(ROOTFS_DIRS),debugfs -w -R "mkdir $(d)" $@ 2>/dev/null || true ;)
	@# ── glibc runtime + all auto-discovered .so deps ──────────────────────────
	@# Writes every .so in RUNTIME_LIB_DIR to lib/x86_64-linux-gnu/, lib/, and
	@# usr/lib/ so ld-linux finds deps regardless of /etc/ld.so.cache presence.
	@# ld-linux itself also lands in lib64/ (canonical PT_INTERP path).
	@for so in $(RUNTIME_LIB_DIR)/*.so*; do \
		[ -f "$$so" ] || continue; \
		n=$$(basename "$$so"); \
		case "$$n" in \
		  ld-linux*) \
			debugfs -w -R "write $$so lib64/$$n" $@ 2>/dev/null; \
			debugfs -w -R "write $$so lib/$$n"   $@ 2>/dev/null; \
			debugfs -w -R "write $$so usr/lib/$$n" $@ 2>/dev/null; \
			;; \
		  *) \
			debugfs -w -R "write $$so lib/x86_64-linux-gnu/$$n" $@ 2>/dev/null; \
			debugfs -w -R "write $$so lib/$$n"                   $@ 2>/dev/null; \
			debugfs -w -R "write $$so usr/lib/$$n"               $@ 2>/dev/null; \
			;; \
		esac; \
	  done
	@# ── coreutils + shell ────────────────────────────────────────────────────
	@for f in $(COREUTILS_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		n=$$(basename "$$f"); \
		debugfs -w -R "write $$f bin/$$n" $@ 2>/dev/null; \
	done
	@# ── system binaries ──────────────────────────────────────────────────────
	debugfs -w -R "write $(SU_BIN)               bin/su"                   $@ 2>/dev/null
	debugfs -w -R "set_inode_field bin/su mode 0104755"                     $@ 2>/dev/null || true
	debugfs -w -R "write $(INIT_BIN)             sbin/init"                $@ 2>/dev/null
	@# ── external binaries ────────────────────────────────────────────────────
	@if [ -d "$(EXTERNAL_BIN_DIR)" ]; then \
		for f in $(EXTERNAL_BIN_DIR)/*; do \
			[ -f "$$f" ] || continue; \
			n=$$(basename "$$f"); \
			debugfs -w -R "write $$f bin/$$n" $@ 2>/dev/null; \
		done; \
	fi
	@# ── rootfs config ────────────────────────────────────────────────────────
	debugfs -w -R "write $(ROOTFS_PROFILE)      etc/profile"               $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_ENV_FILE)     etc/environment"           $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_PASSWD)       etc/passwd"                $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_SHADOW)       etc/shadow"                $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_MAIN_C)       home/root/main.c"              $@ 2>/dev/null
	debugfs -w -R "set_inode_field etc/shadow mode 0100600"                 $@ 2>/dev/null || true
	debugfs -w -R "write $(ROOTFS_GROUP)        etc/group"                 $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_RESOLV)       etc/resolv.conf"           $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_NSSWITCH)     etc/nsswitch.conf"         $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_LD_CONF)      etc/ld.so.conf"            $@ 2>/dev/null
	@if [ -n "$(ROOTFS_CA_BUNDLE)" ] && [ -f "$(ROOTFS_CA_BUNDLE)" ]; then \
		debugfs -w -R "write $(ROOTFS_CA_BUNDLE) etc/ssl/cert.pem" $@ 2>/dev/null; \
		debugfs -w -R "write $(ROOTFS_CA_BUNDLE) etc/ssl/certs/ca-certificates.crt" $@ 2>/dev/null; \
	else \
		echo "!!! warning: host CA bundle not found; HTTPS cert validation may fail in guest"; \
	fi
	@if [ -n "$(ROOTFS_TERMINFO_LINUX)" ] && [ -f "$(ROOTFS_TERMINFO_LINUX)" ]; then \
		debugfs -w -R "write $(ROOTFS_TERMINFO_LINUX) usr/share/terminfo/l/linux" $@ 2>/dev/null; \
	else \
		echo "!!! warning: linux terminfo entry not found on host; nano may fail with TERM=linux"; \
	fi
	@# ── compiler binaries, internals, and header trees ─────────────────────
	@if [ -f "$(COMPILER_DBG_CMDS)" ]; then \
		echo ">>> Writing compiler + headers to root image..."; \
		debugfs -w -f "$(COMPILER_DBG_CMDS)" $@ 2>/dev/null; \
	else \
		echo "!!! warning: compiler not staged; $(COMPILER_DBG_CMDS) missing"; \
	fi

# ── Data partition (ext2) ─────────────────────────────────────────────────────
$(DATA_IMG):
	@echo ">>> Building data partition (ext2)..."
	dd if=/dev/zero of=$@ bs=512 count=$(DATA_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_DATA" -b 4096 $@

# ── GPT disk image ────────────────────────────────────────────────────────────
$(DISK_IMG): $(EFI_IMG) $(ROOT_IMG) $(DATA_IMG)
	@echo ">>> Assembling $(DISK_SIZE_MB) MiB GPT disk image..."
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	sgdisk -Z $@ >/dev/null 2>&1
	sgdisk -n 1:$(EFI_START):$(EFI_END) -t 1:ef00 -c 1:"EFI System" $@ >/dev/null
	sgdisk -n 2:$(ROOT_START):$(ROOT_END) -t 2:8300 -c 2:"Linux Root"  $@ >/dev/null
	sgdisk -n 3:$(DATA_START):$(DATA_END) -t 3:8300 -c 3:"Linux Data"  $@ >/dev/null
	dd if=$(EFI_IMG)  of=$@ bs=512 seek=$(EFI_START)  conv=notrunc 2>/dev/null
	dd if=$(ROOT_IMG) of=$@ bs=512 seek=$(ROOT_START) conv=notrunc 2>/dev/null
	dd if=$(DATA_IMG) of=$@ bs=512 seek=$(DATA_START) conv=notrunc 2>/dev/null
	@echo ">>> Disk image ready: $@"

# ── OVMF vars (writable per-VM copy) ─────────────────────────────────────────
$(OVMF_VARS): $(OVMF_VARS_TMPL)
	cp $< $@

# ── Run ───────────────────────────────────────────────────────────────────────
run: $(DISK_IMG) $(OVMF_VARS)
	qemu-system-x86_64 $(QEMU_FLAGS) -enable-kvm

run-debug: $(DISK_IMG) $(OVMF_VARS)
	qemu-system-x86_64 $(QEMU_FLAGS) -s -S

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)/obj $(KERNEL_ELF) $(DISK_IMG) \
		   $(EFI_IMG) $(ROOT_IMG) $(DATA_IMG) $(BUILD_DIR)/limine.h \
		   $(FONT_DATA_C) $(FONT_DATA_OBJ) $(OVMF_VARS) \
		   $(ROOTFS_STAGE_DIR) $(RUST_TARGET_DIR)

distclean:
	rm -rf $(BUILD_DIR)
