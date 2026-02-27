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

# ── musl userspace smoke binaries ────────────────────────────────────────────
MUSL_DIR       ?= /usr/lib/musl
MUSL_LIB_DIR   ?= $(MUSL_DIR)/lib
MUSL_INC_DIR   ?= $(MUSL_DIR)/include
MUSL_SMOKE_CC  ?= clang

LIBC_SMOKE_DIR  := $(BUILD_DIR)/musl-smoke
LIBC_HELLO_BIN  := $(LIBC_SMOKE_DIR)/hello
LIBC_PTH_BIN    := $(LIBC_SMOKE_DIR)/pthread_smoke
LIBC_SUITE_BIN  := $(LIBC_SMOKE_DIR)/posix_suite
LIBC_SMOKE_BINS := $(LIBC_HELLO_BIN) $(LIBC_PTH_BIN) $(LIBC_SUITE_BIN)

LIBC_HELLO_SRC  := tools/mlibc_hello.c
LIBC_PTH_SRC    := tools/mlibc_pthread_smoke.c
LIBC_SUITE_SRC  := tools/mlibc_posix_suite.c

# ── BusyBox (prebuilt upstream static binary) ─────────────────────────────────
BUSYBOX_URL := https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
BUSYBOX_BIN := $(LIBC_SMOKE_DIR)/busybox

# ── curl (prebuilt static binary) ─────────────────────────────────────────────
CURL_URL := https://github.com/moparisthebest/static-curl/releases/latest/download/curl-amd64
CURL_BIN := $(LIBC_SMOKE_DIR)/curl

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
.PHONY: check-musl libc-smoke busybox-build curl-build

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

# ── musl smoke binaries ──────────────────────────────────────────────────────
check-musl:
	@test -f "$(MUSL_LIB_DIR)/crt1.o" || (echo "Missing musl CRT: $(MUSL_LIB_DIR)/crt1.o"; exit 1)
	@test -f "$(MUSL_INC_DIR)/stdio.h" || (echo "Missing musl headers: $(MUSL_INC_DIR)/stdio.h"; exit 1)

# Usage: $(call musl-link, OUTPUT, SOURCE, LIBS)
define musl-link
	$(MUSL_SMOKE_CC) --target=x86_64-linux-musl \
	    -O2 -static -fno-stack-protector -nostdlib \
	    -isystem $(MUSL_INC_DIR) \
	    $(MUSL_LIB_DIR)/crt1.o \
	    $(MUSL_LIB_DIR)/crti.o \
	    -o $(1) $(2) \
	    -L$(MUSL_LIB_DIR) \
	    -Wl,--start-group $(3) -lgcc -Wl,--end-group \
	    $(MUSL_LIB_DIR)/crtn.o
endef

$(LIBC_HELLO_BIN): $(LIBC_HELLO_SRC) check-musl
	@mkdir -p $(dir $@)
	$(call musl-link,$@,$<,-lc)

$(LIBC_PTH_BIN): $(LIBC_PTH_SRC) check-musl
	@mkdir -p $(dir $@)
	$(call musl-link,$@,$<,-lpthread -lc)

$(LIBC_SUITE_BIN): $(LIBC_SUITE_SRC) check-musl
	@mkdir -p $(dir $@)
	$(call musl-link,$@,$<,-lpthread -lc)

$(BUSYBOX_BIN):
	@echo ">>> Fetching BusyBox static binary..."
	@mkdir -p $(dir $@)
	curl -L --fail -o $@ $(BUSYBOX_URL)
	@chmod +x $@

$(CURL_BIN):
	@echo ">>> Fetching curl static binary..."
	@mkdir -p $(dir $@)
	curl -L --fail -o $@ $(CURL_URL)
	@chmod +x $@

libc-smoke:      $(LIBC_SMOKE_BINS)
busybox-build:   $(BUSYBOX_BIN)
curl-build:      $(CURL_BIN)

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
			 $(LIBC_SMOKE_BINS) $(BUSYBOX_BIN) $(CURL_BIN) \
             $(ROOTFS_PROFILE) $(ROOTFS_ENV_FILE)
	@echo ">>> Building root partition (ext2)..."
	$(eval ROOT_SECTORS := $(shell echo $$(($(DISK_SIZE_MB) * 2048 - $(ROOT_START) - 33))))
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_ROOT" -b 4096 $@
	$(foreach d, dev tmp boot proc sys sys/bus sys/bus/usb sys/bus/usb/devices sys/bus/pci sys/bus/pci/devices home etc bin usr usr/bin usr/sbin sbin var run, \
	    debugfs -w -R "mkdir $(d)" $@ 2>/dev/null || true ;)
	debugfs -w -R "write $(HELLO_BIN)           bin/hello"         $@ 2>/dev/null
	debugfs -w -R "write $(HELLO_BIN)           home/hello"        $@ 2>/dev/null
	debugfs -w -R "write $(SYSCALL_TEST_BIN)    bin/syscall_test"  $@ 2>/dev/null
	debugfs -w -R "write $(SYSCALL_TEST_BIN)    home/syscall_test"  $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_HELLO_BIN)      bin/mlibc_hello"   $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_HELLO_BIN)      home/mlibc_hello"   $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_PTH_BIN)        bin/pthread_smoke" $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_PTH_BIN)        home/pthread_smoke" $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_SUITE_BIN)      bin/posix_suite"   $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_SUITE_BIN)      home/posix_suite"   $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/busybox"       $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/sh"            $@ 2>/dev/null
	debugfs -w -R "write $(CURL_BIN)            bin/curl"          $@ 2>/dev/null
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
	       $(LIBC_SMOKE_DIR) \
	       $(BUILD_DIR)/mlibc $(BUILD_DIR)/mlibc-build $(BUILD_DIR)/sysroot \
	       $(BUILD_DIR)/.stamp-mlibc-*

distclean:
	rm -rf $(BUILD_DIR)