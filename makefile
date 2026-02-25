# EXO_OS Root Makefile
# Uses Limine 8 as UEFI bootloader, boots from a GPT disk image.
# EFI System Partition (FAT32) = /boot  — holds Limine + kernel + config
# Linux Data Partition (ext2)  = /      — root filesystem

ARCH            := x86_64
BUILD_DIR       := build
LIMINE_DIR      := $(BUILD_DIR)/limine
KERNEL_ELF      := $(BUILD_DIR)/exo.elf

# Utility variable — comma is special in make function calls
comma           := ,

LIMINE_TAG      := v8.x-binary
LIMINE_REPO     := https://github.com/limine-bootloader/limine.git

# ── Disk image layout ────────────────────────────────────────────────────────
DISK_IMG        := $(BUILD_DIR)/exo.img
DISK_SIZE_MB    := 256
# Partition 1: EFI System (FAT32) — sectors 2048..133119 = 64 MiB
# 64 MiB ensures >= 65525 clusters for valid FAT32 (UEFI spec compliant)
EFI_START       := 2048
EFI_END         := 133119
EFI_SECTORS     := $(shell echo $$((133119 - 2048 + 1)))
# Partition 2: Linux root (ext2) — sector 133120 to end of disk
ROOT_START      := 133120

# Intermediate partition images
EFI_IMG         := $(BUILD_DIR)/efi.img
ROOT_IMG        := $(BUILD_DIR)/root.img

# OVMF firmware for UEFI boot
OVMF_CODE       := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS_TMPL  := /usr/share/edk2/x64/OVMF_VARS.4m.fd
OVMF_VARS       := $(BUILD_DIR)/OVMF_VARS.4m.fd

# ── Toolchain (Clang + LLD — no cross-compiler needed) ──────────────────────
CC              := clang
LD              := ld.lld
AS              := clang          # for .S files
NASM            := nasm
OBJCOPY         := llvm-objcopy

SRC_DIR         := src/kernel
ARCH_DIR        := $(SRC_DIR)/arch/x86_64

TARGET_TRIPLE   := x86_64-unknown-elf

CFLAGS          :=                      \
    --target=$(TARGET_TRIPLE)           \
    -std=gnu11                          \
    -ffreestanding                      \
    -fno-stack-protector                \
    -fno-stack-check                    \
    -fno-PIC                            \
    -m64                                \
    -march=x86-64                       \
    -mno-80387                          \
    -mno-mmx                            \
    -mno-sse                            \
    -mno-sse2                           \
    -mno-red-zone                       \
    -mcmodel=kernel                     \
    -O2 -g                              \
    -Wall -Wextra                       \
    -Wno-unused-parameter               \
    -I$(SRC_DIR)                        \
    -I$(BUILD_DIR)

ASFLAGS         := $(CFLAGS)
NASMFLAGS       := -f elf64 -g

LDFLAGS         :=                      \
    -m elf_x86_64                       \
    -nostdlib                           \
    -static                             \
    -z max-page-size=0x1000             \
    -T $(SRC_DIR)/linker.ld

# Collect sources
C_SRCS  := $(shell find $(SRC_DIR) -name '*.c')
AS_SRCS := $(shell find $(SRC_DIR) -name '*.S')

# ── Pre-rasterised font (TrueType → C array, host-side tool) ─────────────────
FONT_TOOL     := $(BUILD_DIR)/tools/gen_font
FONT_TTF      := $(SRC_DIR)/gfx/3rdparty/font.ttf
FONT_DATA_C   := $(BUILD_DIR)/gfx_font_data.c
FONT_DATA_OBJ := $(BUILD_DIR)/obj/gfx_font_data.c.o
# Trampoline is special: built as flat binary then wrapped in an ELF object
TRAMPOLINE_SRC  := $(ARCH_DIR)/trampoline.asm
TRAMPOLINE_BIN  := $(BUILD_DIR)/obj/arch/x86_64/trampoline.bin
TRAMPOLINE_OBJ  := $(BUILD_DIR)/obj/arch/x86_64/trampoline.o

# All other NASM sources (everything except trampoline.asm)
NASM_SRCS := $(filter-out $(TRAMPOLINE_SRC), $(shell find $(SRC_DIR) -name '*.asm'))

C_OBJS    := $(patsubst $(SRC_DIR)/%.c,    $(BUILD_DIR)/obj/%.c.o,    $(C_SRCS))
AS_OBJS   := $(patsubst $(SRC_DIR)/%.S,    $(BUILD_DIR)/obj/%.S.o,    $(AS_SRCS))
NASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,  $(BUILD_DIR)/obj/%.asm.o,  $(NASM_SRCS))

ALL_OBJS := $(C_OBJS) $(AS_OBJS) $(NASM_OBJS) $(TRAMPOLINE_OBJ) $(FONT_DATA_OBJ)

.PHONY: all clean run run-debug

all: $(DISK_IMG)

# ─── Download & prepare Limine 8 ───────────────────────────────────────
# Clone the v8.x-binary branch (has prebuilt EFI/BIOS blobs + limine.c)
# Then compile limine.c into the native 'limine' install tool.
$(LIMINE_DIR)/limine.h:
	@echo ">>> Cloning Limine 8 (binary branch)..."
	@mkdir -p $(LIMINE_DIR)
	git clone --branch $(LIMINE_TAG) --depth=1 $(LIMINE_REPO) $(LIMINE_DIR)
	rm -rf $(LIMINE_DIR)/.git
	@echo ">>> Building native limine install tool..."
	cc -o $(LIMINE_DIR)/limine $(LIMINE_DIR)/limine.c

# Copy limine.h to build/ so the kernel can include it
$(BUILD_DIR)/limine.h: $(LIMINE_DIR)/limine.h
	@cp $(LIMINE_DIR)/limine.h $(BUILD_DIR)/limine.h

# ─── Host gen_font tool: pre-rasterise TrueType → C array ───────────────────
$(FONT_TOOL): tools/gen_font.c
	@mkdir -p $(dir $@)
	cc -O2 -o $@ $< -lm

$(FONT_DATA_C): $(FONT_TOOL) $(FONT_TTF)
	@mkdir -p $(dir $@)
	$< $(FONT_TTF) 26 $@

$(FONT_DATA_OBJ): $(FONT_DATA_C) $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ─── Trampoline: flat binary → ELF object ────────────────────────────────────
$(TRAMPOLINE_BIN): $(TRAMPOLINE_SRC)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -o $@ $<

$(TRAMPOLINE_OBJ): $(TRAMPOLINE_BIN)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $< $@

# ─── Compile kernel objects ───────────────────────────────────────────────────
$(BUILD_DIR)/obj/%.c.o: $(SRC_DIR)/%.c $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/%.S.o: $(SRC_DIR)/%.S $(BUILD_DIR)/limine.h
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/%.asm.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ─── Link kernel ─────────────────────────────────────────────────────────────
$(KERNEL_ELF): $(ALL_OBJS)
	@echo ">>> Linking kernel..."
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@

# ─── Build EFI partition image (FAT32 via mtools — no root needed) ────────────
$(EFI_IMG): $(KERNEL_ELF) $(LIMINE_DIR)/limine.h
	@echo ">>> Building EFI partition (FAT32)..."
	dd if=/dev/zero of=$@ bs=512 count=$(EFI_SECTORS) 2>/dev/null
	mkfs.fat -F 32 -n "EFI" $@ >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@ $(LIMINE_DIR)/BOOTX64.EFI   ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(KERNEL_ELF)               ::/boot/exo.elf
	mcopy -i $@ src/boot/limine.conf         ::/boot/limine/limine.conf
	@echo "    EFI partition ready ($(EFI_SECTORS) sectors)"

# ─── Build root partition image (ext2 via mkfs.ext2 + debugfs — no root) ─────
$(ROOT_IMG): $(KERNEL_ELF)
	@echo ">>> Building root partition (ext2)..."
	$(eval ROOT_SECTORS := $(shell echo $$(($(DISK_SIZE_MB) * 2048 - $(ROOT_START) - 33))))
	dd if=/dev/zero of=$@ bs=512 count=$(ROOT_SECTORS) 2>/dev/null
	mkfs.ext2 -q -L "EXOOS_ROOT" -b 4096 $@
	@# Create standard directory hierarchy inside ext2
	debugfs -w -R "mkdir dev"  $@ 2>/dev/null || true
	debugfs -w -R "mkdir tmp"  $@ 2>/dev/null || true
	debugfs -w -R "mkdir boot" $@ 2>/dev/null || true
	debugfs -w -R "mkdir proc" $@ 2>/dev/null || true
	debugfs -w -R "mkdir home" $@ 2>/dev/null || true
	debugfs -w -R "mkdir etc"  $@ 2>/dev/null || true
	@echo "    root partition ready ($(ROOT_SECTORS) sectors)"

# ─── Assemble full GPT disk image ────────────────────────────────────────────
$(DISK_IMG): $(EFI_IMG) $(ROOT_IMG)
	@echo ">>> Assembling $(DISK_SIZE_MB) MiB GPT disk image..."
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	sgdisk -Z $@ >/dev/null 2>&1
	sgdisk -n 1:$(EFI_START):$(EFI_END) -t 1:ef00 -c 1:"EFI System"  $@ >/dev/null
	sgdisk -n 2:$(ROOT_START):0         -t 2:8300 -c 2:"Linux Root"   $@ >/dev/null
	@# Write partition images into the correct offsets
	dd if=$(EFI_IMG)  of=$@ bs=512 seek=$(EFI_START)  conv=notrunc 2>/dev/null
	dd if=$(ROOT_IMG) of=$@ bs=512 seek=$(ROOT_START) conv=notrunc 2>/dev/null
	@echo ">>> Disk image ready: $@"

# ─── Copy OVMF_VARS template (writable per-VM copy) ──────────────────────────
$(OVMF_VARS): $(OVMF_VARS_TMPL)
	@cp $< $@

# ─── Run in QEMU with UEFI (OVMF) ───────────────────────────────────────────
run: $(DISK_IMG) $(OVMF_VARS)
	qemu-system-x86_64                                                  \
	    -machine q35                                                    \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)       \
	    -drive if=pflash,format=raw,file=$(OVMF_VARS)                   \
	    -drive id=hd0,file=$(DISK_IMG),format=raw,if=none               \
	    -device virtio-blk-pci,drive=hd0                                \
	    -m 2G                                                           \
	    -smp 4                                                          \
	    -serial stdio                                                   \
	    -enable-kvm                                                     \
	    -vga std                                                        \
	    -device nec-usb-xhci,id=xhci                                   \
	    -device usb-kbd,bus=xhci.0                                      \
	    -device usb-mouse,bus=xhci.0                                    \
	    -no-reboot

run-debug: $(DISK_IMG) $(OVMF_VARS)
	qemu-system-x86_64                                                  \
	    -machine q35                                                    \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE)       \
	    -drive if=pflash,format=raw,file=$(OVMF_VARS)                   \
	    -drive id=hd0,file=$(DISK_IMG),format=raw,if=none               \
	    -device virtio-blk-pci,drive=hd0                                \
	    -m 2G                                                           \
	    -smp 4                                                          \
	    -serial stdio                                                   \
	    -vga std                                                        \
	    -no-reboot                                                      \
	    -s -S

clean:
	@rm -rf $(BUILD_DIR)/obj $(KERNEL_ELF) $(DISK_IMG)           \
	        $(EFI_IMG) $(ROOT_IMG) $(BUILD_DIR)/limine.h         \
	        $(FONT_DATA_C) $(FONT_DATA_OBJ) $(OVMF_VARS)
	@echo ">>> Cleaned."

distclean:
	@rm -rf $(BUILD_DIR)
	@echo ">>> Full clean done."
