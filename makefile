# EXO_OS Root Makefile
# Limine 8 (UEFI) bootloader — GPT disk image with two partitions:
#   p1: EFI System (FAT32, 64 MiB)  → /boot
#   p2: Linux Root (ext2)            → /

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
DISK_SIZE_MB := 256
# p1: EFI System — 64 MiB; guarantees ≥65525 FAT32 clusters (UEFI spec)
EFI_START    := 2048
EFI_END      := 133119
EFI_SECTORS  := $(shell echo $$(($(EFI_END) - $(EFI_START) + 1)))
# p2: Linux root — sector 133120 to end
ROOT_START   := 133120

EFI_IMG  := $(BUILD_DIR)/efi.img
ROOT_IMG := $(BUILD_DIR)/root.img

# ── OVMF firmware ─────────────────────────────────────────────────────────────
OVMF_CODE      := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS_TMPL := /usr/share/edk2/x64/OVMF_VARS.4m.fd
OVMF_VARS      := $(BUILD_DIR)/OVMF_VARS.4m.fd

# ── Toolchain ─────────────────────────────────────────────────────────────────
CC      := clang
LD      := ld.lld
AS      := clang
NASM    := nasm
OBJCOPY := llvm-objcopy

TARGET_TRIPLE := x86_64-unknown-elf
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
FONT_TOOL     := $(BUILD_DIR)/tools/gen_font
FONT_TTF      := $(SRC_DIR)/gfx/3rdparty/font.ttf
FONT_DATA_C   := $(BUILD_DIR)/gfx_font_data.c
FONT_DATA_OBJ := $(BUILD_DIR)/obj/gfx_font_data.c.o

ALL_OBJS := $(C_OBJS) $(AS_OBJS) $(NASM_OBJS) $(TRAMPOLINE_OBJ) $(FONT_DATA_OBJ)
DEPS     := $(C_OBJS:.o=.d)
-include $(DEPS)

# ── User-space binaries (freestanding, no libc) ───────────────────────────────
HELLO_SRC        := tools/hello.c
HELLO_BIN        := $(BUILD_DIR)/hello
SYSCALL_TEST_SRC := tools/syscall_test.c
SYSCALL_TEST_BIN := $(BUILD_DIR)/syscall_test

USER_CC_FLAGS := \
    --target=x86_64-unknown-linux-elf \
    -nostdlib -nostdinc -static \
    -ffreestanding -fno-stack-protector -fno-PIC -O2 \
    -Wl,-e,_start -Wl,--no-dynamic-linker

# ── mlibc ─────────────────────────────────────────────────────────────────────
MLIBC_REPO    := $(BUILD_DIR)/mlibc
MLIBC_BUILD   := $(BUILD_DIR)/mlibc-build
MLIBC_SYSROOT := $(BUILD_DIR)/sysroot
MLIBC_CROSS   := src/mlibc-sysdeps/exo/exo-cross.ini
MLIBC_SYSDEP  := src/mlibc-sysdeps/exo

MLIBC_CC    ?= clang
MLIBC_CXX   ?= clang++
MLIBC_AR    ?= ar
MLIBC_STRIP ?= strip

MLIBC_SMOKE_DIR  := $(BUILD_DIR)/mlibc-smoke
MLIBC_SMOKE_CC   ?= clang

MLIBC_HELLO_BIN  := $(MLIBC_SMOKE_DIR)/hello
MLIBC_PTH_BIN    := $(MLIBC_SMOKE_DIR)/pthread_smoke
MLIBC_SUITE_BIN  := $(MLIBC_SMOKE_DIR)/posix_suite
MLIBC_SMOKE_BINS := $(MLIBC_HELLO_BIN) $(MLIBC_PTH_BIN) $(MLIBC_SUITE_BIN)

MLIBC_HELLO_SRC  := tools/mlibc_hello.c
MLIBC_PTH_SRC    := tools/mlibc_pthread_smoke.c
MLIBC_SUITE_SRC  := tools/mlibc_posix_suite.c

# Stamp files track the stateful meson steps
MLIBC_STAMP_CLONE     := $(BUILD_DIR)/.stamp-mlibc-clone
MLIBC_STAMP_CONFIGURE := $(BUILD_DIR)/.stamp-mlibc-configure
MLIBC_STAMP_INSTALL   := $(BUILD_DIR)/.stamp-mlibc-install

# ── BusyBox (prebuilt upstream static binary) ─────────────────────────────────
BUSYBOX_URL := https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
BUSYBOX_BIN := $(MLIBC_SMOKE_DIR)/busybox

# ── rootfs config files ───────────────────────────────────────────────────────
ROOTFS_PROFILE  := tools/rootfs/profile
ROOTFS_ENV_FILE := tools/rootfs/environment

# ── QEMU flags (shared between run and run-debug) ─────────────────────────────
QEMU_FLAGS := \
    -machine q35 \
    -cpu host \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(OVMF_VARS) \
    -drive id=hd0,file=$(DISK_IMG),format=raw,if=none \
    -device virtio-blk-pci,drive=hd0 \
    -m 2G \
    -smp 4 \
    -serial stdio \
    -vga std \
    -device nec-usb-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -no-reboot

.PHONY: all clean distclean run run-debug
.PHONY: mlibc-clone mlibc-configure mlibc-build mlibc-install mlibc-smoke busybox-build

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
	$< $(FONT_TTF) 26 $@

$(FONT_DATA_OBJ): $(FONT_DATA_C) $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── User-space binaries ───────────────────────────────────────────────────────
$(HELLO_BIN): $(HELLO_SRC)
	clang $(USER_CC_FLAGS) -o $@ $<

$(SYSCALL_TEST_BIN): $(SYSCALL_TEST_SRC)
	clang $(USER_CC_FLAGS) -o $@ $<

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

$(KERNEL_ELF): $(ALL_OBJS)
	@echo ">>> Linking kernel..."
	$(LD) $(LDFLAGS) $^ -o $@

# ── mlibc build chain ─────────────────────────────────────────────────────────
$(MLIBC_STAMP_CLONE):
	@if [ ! -d "$(MLIBC_REPO)" ]; then \
		echo ">>> Cloning mlibc..."; \
		git clone --depth=1 https://github.com/managarm/mlibc.git $(MLIBC_REPO); \
	fi
	@mkdir -p $(MLIBC_REPO)/sysdeps
	@ln -sfn $(abspath $(MLIBC_SYSDEP)) $(MLIBC_REPO)/sysdeps/exo
	@python3 tools/patch_mlibc_meson.py $(MLIBC_REPO)
	@touch $@

$(MLIBC_STAMP_CONFIGURE): $(MLIBC_STAMP_CLONE)
	@echo ">>> Configuring mlibc..."
	CC=$(MLIBC_CC) CXX=$(MLIBC_CXX) AR=$(MLIBC_AR) STRIP=$(MLIBC_STRIP) \
	meson setup $(MLIBC_BUILD) $(MLIBC_REPO) \
	    --cross-file $(abspath $(MLIBC_CROSS)) \
	    -Dbuild_tests=false \
	    -Dlinux_kernel_headers=disabled \
	    -Ddefault_library=static \
	    --prefix=/usr \
	    --reconfigure
	@touch $@

$(MLIBC_STAMP_INSTALL): $(MLIBC_STAMP_CONFIGURE)
	@echo ">>> Building and installing mlibc..."
	ninja -C $(MLIBC_BUILD)
	@mkdir -p $(MLIBC_SYSROOT)
	DESTDIR=$(abspath $(MLIBC_SYSROOT)) ninja -C $(MLIBC_BUILD) install
	@touch $@

# Shared link recipe for mlibc smoke binaries.
# Usage: $(call mlibc-link, OUTPUT, SOURCE, LIBS)
define mlibc-link
	$(MLIBC_SMOKE_CC) --sysroot=$(abspath $(MLIBC_SYSROOT)) \
	    -O2 -static -fno-stack-protector -nostdlib \
	    $(abspath $(MLIBC_SYSROOT))/usr/lib/crt1.o \
	    $(abspath $(MLIBC_SYSROOT))/usr/lib/crti.o \
	    -o $(1) $(2) \
	    -L$(abspath $(MLIBC_SYSROOT))/usr/lib \
	    -Wl,--start-group $(3) -lgcc -Wl,--end-group \
	    $(abspath $(MLIBC_SYSROOT))/usr/lib/crtn.o
endef

$(MLIBC_HELLO_BIN): $(MLIBC_HELLO_SRC) $(MLIBC_STAMP_INSTALL)
	@mkdir -p $(dir $@)
	$(call mlibc-link,$@,$<,-lc)

$(MLIBC_PTH_BIN): $(MLIBC_PTH_SRC) $(MLIBC_STAMP_INSTALL)
	@mkdir -p $(dir $@)
	$(call mlibc-link,$@,$<,-lpthread -lc)

$(MLIBC_SUITE_BIN): $(MLIBC_SUITE_SRC) $(MLIBC_STAMP_INSTALL)
	@mkdir -p $(dir $@)
	$(call mlibc-link,$@,$<,-lpthread -lc)

$(BUSYBOX_BIN):
	@echo ">>> Fetching BusyBox static binary..."
	@mkdir -p $(dir $@)
	curl -L --fail -o $@ $(BUSYBOX_URL)
	@chmod +x $@

# Phony aliases so manual `make mlibc-smoke` etc. still work
mlibc-clone:     $(MLIBC_STAMP_CLONE)
mlibc-configure: $(MLIBC_STAMP_CONFIGURE)
mlibc-build:     $(MLIBC_STAMP_INSTALL)
mlibc-install:   $(MLIBC_STAMP_INSTALL)
mlibc-smoke:     $(MLIBC_SMOKE_BINS)
busybox-build:   $(BUSYBOX_BIN)

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
$(ROOT_IMG): $(HELLO_BIN) $(SYSCALL_TEST_BIN) \
             $(MLIBC_SMOKE_BINS) $(BUSYBOX_BIN) \
             $(ROOTFS_PROFILE) $(ROOTFS_ENV_FILE)
	@echo ">>> Building root partition (ext2)..."
	$(eval ROOT_SECTORS := $(shell echo $$(($(DISK_SIZE_MB) * 2048 - $(ROOT_START) - 33))))
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_ROOT" -b 4096 $@
	$(foreach d, dev tmp boot proc sys sys/bus sys/bus/usb sys/bus/usb/devices sys/bus/pci sys/bus/pci/devices home etc bin usr usr/bin usr/sbin sbin var run, \
	    debugfs -w -R "mkdir $(d)" $@ 2>/dev/null || true ;)
	debugfs -w -R "write $(HELLO_BIN)           home/hello"        $@ 2>/dev/null
	debugfs -w -R "write $(HELLO_BIN)           bin/hello"         $@ 2>/dev/null
	debugfs -w -R "write $(SYSCALL_TEST_BIN)    bin/syscall_test"  $@ 2>/dev/null
	debugfs -w -R "write $(SYSCALL_TEST_BIN)    home/syscall_test" $@ 2>/dev/null
	debugfs -w -R "write $(MLIBC_HELLO_BIN)     bin/mlibc_hello"   $@ 2>/dev/null
	debugfs -w -R "write $(MLIBC_PTH_BIN)       bin/pthread_smoke" $@ 2>/dev/null
	debugfs -w -R "write $(MLIBC_SUITE_BIN)     bin/posix_suite"   $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/busybox"       $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/ls"            $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/rm"            $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/mkdir"         $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/sh"            $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_PROFILE)      etc/profile"       $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_ENV_FILE)     etc/environment"   $@ 2>/dev/null

# ── GPT disk image ────────────────────────────────────────────────────────────
$(DISK_IMG): $(EFI_IMG) $(ROOT_IMG)
	@echo ">>> Assembling $(DISK_SIZE_MB) MiB GPT disk image..."
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	sgdisk -Z $@ >/dev/null 2>&1
	sgdisk -n 1:$(EFI_START):$(EFI_END) -t 1:ef00 -c 1:"EFI System" $@ >/dev/null
	sgdisk -n 2:$(ROOT_START):0         -t 2:8300 -c 2:"Linux Root"  $@ >/dev/null
	dd if=$(EFI_IMG)  of=$@ bs=512 seek=$(EFI_START)  conv=notrunc 2>/dev/null
	dd if=$(ROOT_IMG) of=$@ bs=512 seek=$(ROOT_START) conv=notrunc 2>/dev/null
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
	       $(EFI_IMG) $(ROOT_IMG) $(BUILD_DIR)/limine.h \
	       $(FONT_DATA_C) $(FONT_DATA_OBJ) $(OVMF_VARS) \
	       $(MLIBC_BUILD) $(MLIBC_SYSROOT) $(MLIBC_SMOKE_DIR) \
	       $(BUILD_DIR)/.stamp-mlibc-*

distclean:
	rm -rf $(BUILD_DIR)