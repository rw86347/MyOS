# =============================================================================
# MyOS Makefile
#
#   make            build build/myos.img (bootable raw disk image)
#   make run        boot in QEMU: VGA in a window, serial output in this terminal
#   make run-text   boot in QEMU fully in the terminal (VGA text screen via curses;
#                   serial goes to build/serial.log). Quit: Esc then 2, type "quit".
#   make debug      like run, but paused at the first instruction; then in another
#                   terminal: gdb build/kernel.elf -ex "target remote localhost:1234"
#   make test       boot headless, print the serial log, fail unless self-tests pass
#   make usb        (re)build build/usb.img, the FAT32 test disk (needs mtools)
#   make clean      delete build/
#
# QEMU gets two IDE disks:  hda = myos.img (boot)   hdb = usb.img (FAT32 data)
#
# How the image is put together:
#   kernel/*.c + drivers/*.c + arch/i386/entry.asm  --link-->  build/kernel.elf
#   kernel.elf  --objcopy-->  build/kernel.bin   (raw bytes, loaded at 0x10000)
#   arch/i386/boot/mbr.asm  --nasm-->  build/mbr.bin   (exactly 512 bytes)
#   myos.img = [ mbr.bin | kernel.bin | zero padding ]
# =============================================================================

CC      := i686-elf-gcc
OBJCOPY := i686-elf-objcopy
NASM    := nasm
QEMU    := qemu-system-i386

BUILD   := build

# Freestanding: no host OS, no C library. Only compiler-provided headers like
# <stdint.h> and <stddef.h> are available.
# -fno-tree-loop-distribute-patterns: stop GCC from turning our memcpy/memset
# loops into calls to memcpy/memset — inside memcpy itself that would recurse.
CFLAGS  := -std=c11 -ffreestanding -O2 -Wall -Wextra -g \
           -fno-tree-loop-distribute-patterns \
           -Iinclude -Iarch/i386/include
# --no-warn-rwx-segments: the kernel is one read/write/execute blob for now;
# we'll split code and data permissions once paging is on (roadmap step 3).
LDFLAGS := -T arch/i386/linker.ld -nostdlib -ffreestanding -O2 -Wl,--no-warn-rwx-segments
LIBS    := -lgcc    # gcc's helper routines (e.g. 64-bit division on i386)

C_SRCS  := $(wildcard kernel/*.c drivers/*.c fs/*.c)
C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
# entry.o must be linked first (it holds _start; linker.ld also enforces this)
OBJS    := $(BUILD)/arch/i386/entry.o $(C_OBJS)

IMG     := $(BUILD)/myos.img
USB     := $(BUILD)/usb.img
QEMU_FLAGS := -m 64M \
              -drive format=raw,file=$(IMG),index=0,media=disk \
              -drive format=raw,file=$(USB),index=1,media=disk

.PHONY: all run run-text debug test usb clean

all: $(IMG) $(USB)

# --- kernel ------------------------------------------------------------------
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch/i386/entry.o: arch/i386/entry.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) arch/i386/linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

# Strip the ELF wrapper: the MBR just copies raw bytes into memory.
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# --- bootloader --------------------------------------------------------------
# The MBR needs to know how many 512-byte sectors to read, so we round the
# kernel size up to whole sectors and pass it to nasm.
$(BUILD)/mbr.bin: arch/i386/boot/mbr.asm $(BUILD)/kernel.bin
	$(NASM) -f bin \
	  -DKERNEL_SECTORS=$$(( ($$(wc -c < $(BUILD)/kernel.bin) + 511) / 512 )) \
	  $< -o $@

# --- disk image --------------------------------------------------------------
# 1.44 MB (2880 sectors) of zeros, then the MBR at sector 0 and the kernel
# starting at sector 1. "conv=notrunc" overwrites without shrinking the file.
$(IMG): $(BUILD)/mbr.bin $(BUILD)/kernel.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(BUILD)/mbr.bin    of=$@ bs=512 seek=0 conv=notrunc 2>/dev/null
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	@echo "Built $@ (kernel: $$(wc -c < $(BUILD)/kernel.bin | tr -d ' ') bytes)"

# --- FAT32 test disk ---------------------------------------------------------
$(USB): tools/mkusb.sh
	tools/mkusb.sh $@

usb:
	tools/mkusb.sh $(USB)

# --- running -----------------------------------------------------------------
run: $(IMG) $(USB)
	$(QEMU) $(QEMU_FLAGS) -serial stdio

run-text: $(IMG) $(USB)
	$(QEMU) $(QEMU_FLAGS) -display curses -serial file:$(BUILD)/serial.log

debug: $(IMG) $(USB)
	$(QEMU) $(QEMU_FLAGS) -serial stdio -s -S

# Boot with no display, give the kernel 5 seconds, then tell QEMU to quit
# through its monitor. The kernel's serial output is the test result.
test: $(IMG) $(USB)
	@rm -f $(BUILD)/serial.log
	@(sleep 5; echo quit) | $(QEMU) $(QEMU_FLAGS) -display none \
	  -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@cat $(BUILD)/serial.log
	@grep -q "Self-tests: [0-9]* passed, 0 failed" $(BUILD)/serial.log \
	  && echo "TEST OK" || (echo "TEST FAILED"; exit 1)

clean:
	rm -rf $(BUILD)
