# EXO_OS Root Makefile
# Uses Limine 8 as bootloader, downloaded into build/limine

ARCH            := x86_64
BUILD_DIR       := build
ISO_DIR         := $(BUILD_DIR)/iso_root
LIMINE_DIR      := $(BUILD_DIR)/limine
KERNEL_ELF      := $(BUILD_DIR)/exo.elf
ISO_IMAGE       := $(BUILD_DIR)/exo.iso

LIMINE_TAG      := v8.x-binary
LIMINE_REPO     := https://github.com/limine-bootloader/limine.git

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
# Trampoline is special: built as flat binary then wrapped in an ELF object
TRAMPOLINE_SRC  := $(ARCH_DIR)/trampoline.asm
TRAMPOLINE_BIN  := $(BUILD_DIR)/obj/arch/x86_64/trampoline.bin
TRAMPOLINE_OBJ  := $(BUILD_DIR)/obj/arch/x86_64/trampoline.o

# All other NASM sources (everything except trampoline.asm)
NASM_SRCS := $(filter-out $(TRAMPOLINE_SRC), $(shell find $(SRC_DIR) -name '*.asm'))

C_OBJS    := $(patsubst $(SRC_DIR)/%.c,    $(BUILD_DIR)/obj/%.c.o,    $(C_SRCS))
AS_OBJS   := $(patsubst $(SRC_DIR)/%.S,    $(BUILD_DIR)/obj/%.S.o,    $(AS_SRCS))
NASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,  $(BUILD_DIR)/obj/%.asm.o,  $(NASM_SRCS))

ALL_OBJS := $(C_OBJS) $(AS_OBJS) $(NASM_OBJS) $(TRAMPOLINE_OBJ)

.PHONY: all clean run

all: $(ISO_IMAGE)

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

# ─── Build ISO ────────────────────────────────────────────────────────────────
$(ISO_IMAGE): $(KERNEL_ELF) $(LIMINE_DIR)/limine.h
	@echo ">>> Building ISO..."
	@mkdir -p $(ISO_DIR)/boot/limine
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	@cp $(KERNEL_ELF)                         $(ISO_DIR)/boot/exo.elf
	@cp src/boot/limine.conf                  $(ISO_DIR)/boot/limine/limine.conf
	@cp $(LIMINE_DIR)/limine-bios.sys         $(ISO_DIR)/boot/limine/
	@cp $(LIMINE_DIR)/limine-bios-cd.bin      $(ISO_DIR)/boot/limine/
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin      $(ISO_DIR)/boot/limine/
	@cp $(LIMINE_DIR)/BOOTX64.EFI             $(ISO_DIR)/EFI/BOOT/
	xorriso -as mkisofs                                             \
	    -b       boot/limine/limine-bios-cd.bin                    \
	    -no-emul-boot -boot-load-size 4 -boot-info-table           \
	    --efi-boot boot/limine/limine-uefi-cd.bin                  \
	    -efi-boot-part --efi-boot-image --protective-msdos-label   \
	    $(ISO_DIR) -o $(ISO_IMAGE)
	$(LIMINE_DIR)/limine bios-install $(ISO_IMAGE)
	@echo ">>> ISO ready: $(ISO_IMAGE)"

# ─── Run in QEMU ─────────────────────────────────────────────────────────────
run: $(ISO_IMAGE)
	qemu-system-x86_64          \
	    -cdrom $(ISO_IMAGE)     \
	    -m 2G                 \
	    -smp 4                  \
	    -serial stdio           \
	    -enable-kvm             \
	    -no-reboot

run-debug: $(ISO_IMAGE)
	qemu-system-x86_64          \
	    -cdrom $(ISO_IMAGE)     \
	    -m 512M                 \
	    -smp 4                  \
	    -serial stdio           \
	    -no-reboot              \
	    -s -S

clean:
	@rm -rf $(BUILD_DIR)/obj $(KERNEL_ELF) $(ISO_IMAGE) \
	        $(ISO_DIR) $(BUILD_DIR)/limine.h
	@echo ">>> Cleaned."

distclean:
	@rm -rf $(BUILD_DIR)
	@echo ">>> Full clean done."
