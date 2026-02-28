# EXO_OS Root Makefile
# Limine 8 (UEFI) bootloader — GPT disk image with three partitions:
#   p1: EFI System (FAT32, 64 MiB)  → /boot
#   p2: Linux Root (ext2)            → /
#   p3: Linux Data (ext2)            → /mnt

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
# p2: Linux root — fixed-size partition
ROOT_START    := 133120
ROOT_SIZE_MB  := 128
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

# ── musl userspace dynamic binaries ──────────────────────────────────────────
MUSL_DIR       ?= /usr/lib/musl
MUSL_LIB_DIR   ?= $(MUSL_DIR)/lib
MUSL_INC_DIR   ?= $(MUSL_DIR)/include
MUSL_SMOKE_CC  ?= clang
MUSL_LD_SO     ?= $(shell sh -c 'for p in /lib/ld-musl-x86_64.so.1 /usr/lib/ld-musl-x86_64.so.1 /usr/lib/musl/lib/ld-musl-x86_64.so.1; do [ -f "$$p" ] && { echo "$$p"; exit 0; }; done')

LIBC_SMOKE_DIR  := $(BUILD_DIR)/musl-smoke
LIBC_HELLO_DYN_BIN := $(LIBC_SMOKE_DIR)/hello_dynamic
LIBC_DLOPEN_TEST_BIN := $(LIBC_SMOKE_DIR)/dlopen_test
LIBC_LIBTEST_SO := $(LIBC_SMOKE_DIR)/libtest.so
LIBC_DYNAMIC_BINS := $(LIBC_HELLO_DYN_BIN) $(LIBC_DLOPEN_TEST_BIN)
LIBC_DYNAMIC_LIBS := $(LIBC_LIBTEST_SO)

LIBC_HELLO_DYN_SRC := tools/hello_dynamic.c
LIBC_DLOPEN_TEST_SRC := tools/dlopen_test.c
LIBC_LIBTEST_SO_SRC := tools/libtest.c

MUSL_RUNTIME_DIR := $(LIBC_SMOKE_DIR)/lib
MUSL_RUNTIME_STAMP := $(MUSL_RUNTIME_DIR)/.stamp

# ── BusyBox (prebuilt upstream static binary) ─────────────────────────────────
BUSYBOX_URL := https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
BUSYBOX_BIN := $(LIBC_SMOKE_DIR)/busybox

# ── External binaries (configured in script) ─────────────────────────────────
EXTERNAL_BIN_LIST_SH := tools/rootfs/external_bins.sh
EXTERNAL_BIN_DIR     := $(LIBC_SMOKE_DIR)/external-bin
EXTERNAL_BIN_STAMP   := $(EXTERNAL_BIN_DIR)/.stamp

# ── rootfs config files ───────────────────────────────────────────────────────
ROOTFS_PROFILE  := tools/rootfs/profile
ROOTFS_ENV_FILE := tools/rootfs/environment
ROOTFS_PASSWD   := tools/rootfs/passwd
ROOTFS_GROUP    := tools/rootfs/group
ROOTFS_RESOLV   := tools/rootfs/resolv.conf
ROOTFS_TERMINFO_LINUX := $(shell sh -c 'for p in /usr/share/terminfo/l/linux /lib/terminfo/l/linux /etc/terminfo/l/linux; do [ -f "$$p" ] && { echo "$$p"; exit 0; }; done')
ROOTFS_CA_BUNDLE := $(shell sh -c 'for p in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do [ -f "$$p" ] && { echo "$$p"; exit 0; }; done')

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
.PHONY: check-musl check-musl-dynamic libc-smoke busybox-build external-bins-build curl-build

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
	$< $(FONT_TTF) 20 $@

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

$(KERNEL_ELF): $(ALL_OBJS)
	@echo ">>> Linking kernel..."
	$(LD) $(LDFLAGS) $^ -o $@

# ── musl dynamic binaries ────────────────────────────────────────────────────
check-musl:
	@test -f "$(MUSL_LIB_DIR)/crt1.o" || (echo "Missing musl CRT: $(MUSL_LIB_DIR)/crt1.o"; exit 1)
	@test -f "$(MUSL_INC_DIR)/stdio.h" || (echo "Missing musl headers: $(MUSL_INC_DIR)/stdio.h"; exit 1)

check-musl-dynamic:
	@test -n "$(MUSL_LD_SO)" || (echo "Missing dynamic musl loader (ld-musl-x86_64.so.1)"; exit 1)
	@test -f "$(MUSL_LD_SO)" || (echo "Missing dynamic musl loader file: $(MUSL_LD_SO)"; exit 1)

# Usage: $(call musl-link-dynamic, OUTPUT, SOURCE, LIBS)
define musl-link-dynamic
	$(MUSL_SMOKE_CC) --target=x86_64-linux-musl \
	    -O2 -fno-stack-protector -nostdlib \
	    -isystem $(MUSL_INC_DIR) \
	    $(MUSL_LIB_DIR)/crt1.o \
	    $(MUSL_LIB_DIR)/crti.o \
	    -o $(1) $(2) \
	    -L$(MUSL_LIB_DIR) \
	    -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
	    -Wl,--start-group $(3) -lgcc -Wl,--end-group \
	    $(MUSL_LIB_DIR)/crtn.o
endef

# Usage: $(call musl-link-shared, OUTPUT, SOURCE, LIBS)
define musl-link-shared
	$(MUSL_SMOKE_CC) --target=x86_64-linux-musl \
	    -O2 -fPIC -shared \
	    -isystem $(MUSL_INC_DIR) \
	    -o $(1) $(2) \
	    -L$(MUSL_LIB_DIR) \
	    $(3)
endef

$(LIBC_HELLO_DYN_BIN): $(LIBC_HELLO_DYN_SRC) check-musl check-musl-dynamic
	@mkdir -p $(dir $@)
	$(call musl-link-dynamic,$@,$<,-lc)

$(LIBC_DLOPEN_TEST_BIN): $(LIBC_DLOPEN_TEST_SRC) check-musl check-musl-dynamic
	@mkdir -p $(dir $@)
	$(call musl-link-dynamic,$@,$<,-ldl -lc)

$(LIBC_LIBTEST_SO): $(LIBC_LIBTEST_SO_SRC) check-musl check-musl-dynamic
	@mkdir -p $(dir $@)
	$(call musl-link-shared,$@,$<,-lc)

$(MUSL_RUNTIME_STAMP): check-musl-dynamic
	@mkdir -p $(MUSL_RUNTIME_DIR)
	cp $(MUSL_LD_SO) $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1
	cp $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1 $(MUSL_RUNTIME_DIR)/libc.so
	cp $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1 $(MUSL_RUNTIME_DIR)/libdl.so
	cp $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1 $(MUSL_RUNTIME_DIR)/libpthread.so
	cp $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1 $(MUSL_RUNTIME_DIR)/libm.so
	@touch $@

$(BUSYBOX_BIN):
	@echo ">>> Fetching BusyBox static binary..."
	@mkdir -p $(dir $@)
	curl -L --fail -o $@ $(BUSYBOX_URL)
	@chmod +x $@

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

libc-smoke:      $(LIBC_DYNAMIC_BINS) $(LIBC_DYNAMIC_LIBS) $(MUSL_RUNTIME_STAMP)
busybox-build:   $(BUSYBOX_BIN)
external-bins-build: $(EXTERNAL_BIN_STAMP)
curl-build: external-bins-build

# ── init binary (musl static — installed as /sbin/init in the root image) ────
INIT_SRC := tools/init.c
INIT_BIN := $(BUILD_DIR)/init

$(INIT_BIN): $(INIT_SRC) check-musl
	@mkdir -p $(dir $@)
	$(MUSL_SMOKE_CC) --target=x86_64-linux-musl \
	    -O2 -fno-stack-protector -static -nostdlib \
	    -isystem $(MUSL_INC_DIR) \
	    $(MUSL_LIB_DIR)/crt1.o \
	    $(MUSL_LIB_DIR)/crti.o \
	    -o $@ $< \
	    -L$(MUSL_LIB_DIR) \
	    -Wl,--start-group -lc -lgcc -Wl,--end-group \
	    $(MUSL_LIB_DIR)/crtn.o

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
$(ROOT_IMG): $(LIBC_DYNAMIC_BINS) $(LIBC_DYNAMIC_LIBS) $(MUSL_RUNTIME_STAMP) \
			 $(BUSYBOX_BIN) $(EXTERNAL_BIN_STAMP) $(INIT_BIN) \
		     $(ROOTFS_PROFILE) $(ROOTFS_ENV_FILE) $(ROOTFS_PASSWD) $(ROOTFS_GROUP) $(ROOTFS_RESOLV)
	@echo ">>> Building root partition (ext2)..."
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_ROOT" -b 4096 $@
	$(foreach d, dev tmp boot proc sys sys/bus sys/bus/usb sys/bus/usb/devices sys/bus/pci sys/bus/pci/devices home home/root etc etc/ssl etc/ssl/certs bin lib usr usr/bin usr/sbin usr/share usr/share/terminfo usr/share/terminfo/l sbin var run mnt, \
	    debugfs -w -R "mkdir $(d)" $@ 2>/dev/null || true ;)
	debugfs -w -R "write $(LIBC_HELLO_DYN_BIN)  home/root/hello_dynamic"   $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_DLOPEN_TEST_BIN)  home/root/dlopen_test"   $@ 2>/dev/null
	debugfs -w -R "write $(LIBC_LIBTEST_SO)  lib/libtest.so"   $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/busybox"       $@ 2>/dev/null
	debugfs -w -R "write $(BUSYBOX_BIN)         bin/sh"            $@ 2>/dev/null
	debugfs -w -R "write $(INIT_BIN)            sbin/init"         $@ 2>/dev/null
	debugfs -w -R "write $(MUSL_RUNTIME_DIR)/ld-musl-x86_64.so.1 lib/ld-musl-x86_64.so.1" $@ 2>/dev/null
	debugfs -w -R "write $(MUSL_RUNTIME_DIR)/libc.so lib/libc.so" $@ 2>/dev/null
	debugfs -w -R "write $(MUSL_RUNTIME_DIR)/libdl.so lib/libdl.so" $@ 2>/dev/null
	debugfs -w -R "write $(MUSL_RUNTIME_DIR)/libpthread.so lib/libpthread.so" $@ 2>/dev/null
	debugfs -w -R "write $(MUSL_RUNTIME_DIR)/libm.so lib/libm.so" $@ 2>/dev/null
	@if [ -d "$(EXTERNAL_BIN_DIR)" ]; then \
	    for f in $(EXTERNAL_BIN_DIR)/*; do \
	        [ -f "$$f" ] || continue; \
	        n=$$(basename "$$f"); \
	        debugfs -w -R "write $$f bin/$$n" $@ 2>/dev/null; \
	    done; \
	fi
	debugfs -w -R "write $(ROOTFS_PROFILE)      etc/profile"       $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_ENV_FILE)     etc/environment"   $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_PASSWD)       etc/passwd"        $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_GROUP)        etc/group"         $@ 2>/dev/null
	debugfs -w -R "write $(ROOTFS_RESOLV)       etc/resolv.conf"   $@ 2>/dev/null
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
	       $(LIBC_SMOKE_DIR)

distclean:
	rm -rf $(BUILD_DIR)
