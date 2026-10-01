# =============================================================================
# MyOS Makefile
#
#   make            build build/myos.img (bootable raw disk image)
#   make run        boot in QEMU: VGA in a window, serial output in this terminal
#   make run-text   boot in QEMU fully in the terminal (VGA text screen via curses;
#                   serial goes to build/serial.log). Quit: Esc then 2, type "quit".
#   make debug      like run, but paused at the first instruction; then in another
#                   terminal: gdb build/kernel.elf -ex "target remote localhost:1234"
#   make test       build with self-tests (build/test/), boot headless, print the
#                   serial log, fail unless every check passes
#   make bench      build with benchmarks (build/bench/), boot headless, print cycles
#   make usb        (re)build build/usb.img, the FAT32 test disk (needs mtools)
#   make clean      delete build/
#
# Build modes: a normal build contains no test or benchmark code at all.
# `make run MODE=test` boots the self-test build in a window.
#
# QEMU gets two IDE disks:  hda = myos.img (boot)   hdb = usb.img (FAT32 data)
#
# How the image is put together:
#   kernel/*.c + drivers/*.c + arch/i386/entry.asm  --link-->  build/kernel.elf
#   kernel.elf  --objcopy-->  build/kernel.bin   (raw bytes, loaded at 1 MiB)
#   initramfs/  --cpio-->  build/initrd.cpio      (loaded right after the kernel)
#   arch/i386/boot/mbr.asm     --nasm-->  build/mbr.bin     (exactly 512 bytes)
#   arch/i386/boot/stage2.asm  --nasm-->  build/stage2.bin  (STAGE2_SECTORS)
#   myos.img = [ mbr.bin | stage2.bin | kernel.bin | initrd.cpio | padding ]
# =============================================================================

CC      := i686-elf-gcc
OBJCOPY := i686-elf-objcopy
LD      := i686-elf-ld
NM      := i686-elf-nm
NASM    := nasm
QEMU    := qemu-system-i386

# --- build modes ---------------------------------------------------------------
MODE ?= normal
ifeq ($(MODE),test)
  BUILD      := build/test
  MODE_FLAGS := -DSELFTEST
else ifeq ($(MODE),bench)
  BUILD      := build/bench
  MODE_FLAGS := -DBENCH
else
  BUILD      := build
  MODE_FLAGS :=
endif

# Freestanding: no host OS, no C library. Only compiler-provided headers like
# <stdint.h> and <stddef.h> are available.
# -fno-tree-loop-distribute-patterns: stop GCC from turning our memcpy/memset
# loops into calls to memcpy/memset — inside memcpy itself that would recurse.
# -mgeneral-regs-only: the kernel never uses FPU/SSE registers (no floats, and
# no SSE tricks in copies), so kernel threads never need them saved (fpu.c).
# -march=i686: Pentium Pro instruction set (cmov etc.), which every target has.
CFLAGS  := -std=c11 -ffreestanding -O2 -Wall -Wextra -g -march=i686 \
           -fno-tree-loop-distribute-patterns -mgeneral-regs-only \
           -Iinclude -Iarch/i386/include $(MODE_FLAGS)
# --no-warn-rwx-segments: the kernel is one read/write/execute blob for now
# (code isn't write-protected; see docs/gaps.md).
LDFLAGS := -T arch/i386/linker.ld -nostdlib -ffreestanding -O2 -Wl,--no-warn-rwx-segments
LIBS    := -lgcc    # gcc's helper routines (e.g. 64-bit division on i386)

# Test and benchmark code goes only into its own build mode.
TEST_SRCS  := kernel/selftest.c kernel/disk_demo.c $(wildcard kernel/*_test.c)
BENCH_SRCS := kernel/bench.c
C_SRCS  := $(filter-out $(TEST_SRCS) $(BENCH_SRCS), \
             $(wildcard arch/i386/*.c kernel/*.c drivers/*.c fs/*.c mm/*.c))
ifeq ($(MODE),test)
  C_SRCS += $(TEST_SRCS)
endif
ifeq ($(MODE),bench)
  C_SRCS += $(BENCH_SRCS)
endif
C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
# Kernel assembly files (the MBR in arch/i386/boot/ is built separately).
ASM_SRCS := $(filter-out arch/i386/entry.asm,$(wildcard arch/i386/*.asm))
ASM_OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))
# entry.o must be linked first (it holds _start; linker.ld also enforces this)
# Generated from config/slots.conf at build time (see below).
GEN_OBJS := $(BUILD)/gen/slots_conf.o
# User test programs (user/*.asm and user/*.c -> ELF executables). Test and
# bench builds put them in the initramfs as /bin/NAME.
USER_BINS := $(patsubst user/%.asm,$(BUILD)/user/%.elf,$(wildcard user/*.asm)) \
             $(patsubst user/%.c,$(BUILD)/user/%.elf,$(wildcard user/*.c))
USER_LIB  := $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/mini.o
OBJS    := $(BUILD)/arch/i386/entry.o $(ASM_OBJS) $(C_OBJS) $(GEN_OBJS)

IMG     := $(BUILD)/myos.img
INITRD  := $(BUILD)/initrd.cpio
# 4 KiB reserved for stage 2 on the disk (layout.inc gets it with -D)
STAGE2_SECTORS := 8
USB     := build/usb.img
# -cpu pentium3: the features both target CPUs (Pentium 4, Pentium M) have —
# so we never rely on anything the old machines lack.
QEMU_FLAGS := -m 64M -cpu pentium3 \
              -drive format=raw,file=$(IMG),index=0,media=disk \
              -drive format=raw,file=$(USB),index=1,media=disk

.PHONY: all run run-text debug test check-selftests bench run-bench usb clean

all: $(IMG) $(USB)

# --- user programs -----------------------------------------------------------
# (These rules come before the kernel's: make 3.81 uses the first pattern
# rule that matches, and user code must not get the kernel's flags.)
# Each user/NAME.asm or user/NAME.c becomes an i386 ELF executable, linked at
# 0x08048000 like Linux programs.
USER_CFLAGS := -std=c11 -ffreestanding -O2 -Wall -Wextra -march=i686 -Iuser/lib

$(BUILD)/user/lib/%.o: user/lib/%.c user/lib/mini.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/lib/%.o: user/lib/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/user/%.elf: user/%.c $(USER_LIB) user/lib/mini.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $(BUILD)/user/$*.o
	$(CC) -nostdlib -static $(USER_LIB) $(BUILD)/user/$*.o -o $@ -lgcc

$(BUILD)/user/%.elf: user/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $(BUILD)/user/$*.o
	$(LD) $(BUILD)/user/$*.o -o $@

# --- kernel ------------------------------------------------------------------
# -MMD -MP: also write a .d file listing the headers each .c includes, so
# changing a header rebuilds every file that uses it.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

-include $(C_OBJS:.o=.d)

# --- time-slot table ---------------------------------------------------------
# The kernel can't read files yet, so config/slots.conf becomes a C array.
# Write to .tmp first so a config error doesn't leave a half-written file.
$(BUILD)/gen/slots_conf.c: config/slots.conf tools/slots2c.awk
	@mkdir -p $(dir $@)
	awk -f tools/slots2c.awk $< > $@.tmp && mv $@.tmp $@

$(BUILD)/gen/slots_conf.o: $(BUILD)/gen/slots_conf.c include/sched.h include/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) arch/i386/linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

# Strip the ELF wrapper: stage 2 just copies raw bytes into memory.
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# --- initramfs ---------------------------------------------------------------
# Everything in initramfs/ is packed into a cpio archive ("newc" format, the
# one Linux uses), owned by root. Test and bench builds add the test
# programs as /bin/NAME, and data/numbers.txt: ~110 KB, so stage 2 has to
# load the archive in several 32 KiB chunks.
INITRD_FILES := $(shell find initramfs -type f 2>/dev/null)

$(INITRD): $(INITRD_FILES) $(if $(MODE_FLAGS),$(USER_BINS)) Makefile
	@rm -rf $(BUILD)/initramfs
	@mkdir -p $(BUILD)/initramfs
	cp -R initramfs/. $(BUILD)/initramfs/
ifneq ($(MODE_FLAGS),)
	@mkdir -p $(BUILD)/initramfs/bin $(BUILD)/initramfs/data
	for f in $(USER_BINS); do cp $$f $(BUILD)/initramfs/bin/$$(basename $$f .elf); done
	seq 1 20000 > $(BUILD)/initramfs/data/numbers.txt
endif
	cd $(BUILD)/initramfs && find . -mindepth 1 ! -name .DS_Store | sort \
	  | cpio -o -H newc -R 0:0 --quiet > ../initrd.cpio

# --- bootloader --------------------------------------------------------------
# Size of a file in 512-byte sectors, rounded up (worked out by the shell).
sectors = $$(( ($$(wc -c < $(1)) + 511) / 512 ))

# The MBR only loads stage 2, which always has the same size.
$(BUILD)/mbr.bin: arch/i386/boot/mbr.asm arch/i386/boot/layout.inc
	@mkdir -p $(dir $@)
	$(NASM) -f bin -i arch/i386/boot/ -DSTAGE2_SECTORS=$(STAGE2_SECTORS) $< -o $@

# Stage 2 needs to know where the kernel and the initramfs are on the disk,
# and where the initramfs may go in RAM: the first page after the kernel's
# end in memory (__kernel_end, which includes .bss — kernel.bin doesn't).
$(BUILD)/stage2.bin: arch/i386/boot/stage2.asm arch/i386/boot/layout.inc \
                     $(BUILD)/kernel.bin $(INITRD)
	K=$(call sectors,$(BUILD)/kernel.bin); \
	I=$(call sectors,$(INITRD)); \
	END=0x$$($(NM) $(BUILD)/kernel.elf | awk '$$3 == "__kernel_end" { print $$1 }'); \
	$(NASM) -f bin -i arch/i386/boot/ -DSTAGE2_SECTORS=$(STAGE2_SECTORS) \
	  -DKERNEL_LBA=$$((1 + $(STAGE2_SECTORS))) -DKERNEL_SECTORS=$$K \
	  -DINITRD_LBA=$$((1 + $(STAGE2_SECTORS) + K)) -DINITRD_SECTORS=$$I \
	  -DINITRD_BYTES=$$(( $$(wc -c < $(INITRD)) )) \
	  -DINITRD_LOAD=$$(( (END - 0xC0000000 + 4095) / 4096 * 4096 )) \
	  $< -o $@

# --- disk image --------------------------------------------------------------
# 1.44 MB of zeros (it grows if the pieces need more), then the pieces one
# after another. "conv=notrunc" overwrites without shrinking the file;
# "sync" pads the last partial sector with zeros.
$(IMG): $(BUILD)/mbr.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin $(INITRD)
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(BUILD)/mbr.bin    of=$@ bs=512 seek=0 conv=notrunc 2>/dev/null
	dd if=$(BUILD)/stage2.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=$$((1 + $(STAGE2_SECTORS))) \
	  conv=notrunc,sync 2>/dev/null
	K=$(call sectors,$(BUILD)/kernel.bin); \
	dd if=$(INITRD) of=$@ bs=512 seek=$$((1 + $(STAGE2_SECTORS) + K)) \
	  conv=notrunc,sync 2>/dev/null
	@echo "Built $@ (kernel: $$(wc -c < $(BUILD)/kernel.bin | tr -d ' ') bytes," \
	  "initramfs: $$(wc -c < $(INITRD) | tr -d ' ') bytes)"

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

# Boot with no display and drive it through QEMU's monitor: wait for boot,
# "type" h, i, Enter, press F10 (NULL pointer -> page fault), then quit.
# The kernel's serial output is the test result; each grep checks one thing.
test:
	@$(MAKE) --no-print-directory MODE=test check-selftests

check-selftests: $(IMG) $(USB)
	@rm -f $(BUILD)/serial.log
	@(sleep 4; for k in h i ret; do echo "sendkey $$k"; sleep 0.2; done; \
	  echo "sendkey f10"; sleep 1; echo quit) \
	  | $(QEMU) $(QEMU_FLAGS) -display none \
	    -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@cat $(BUILD)/serial.log; echo
	@L=$(BUILD)/serial.log; ok=1; \
	  grep -q "Timer: 1000 Hz (sleep 100 ms = 10[0-9] ticks)" $$L || { echo "FAIL: timer"; ok=0; }; \
	  grep -q "Self-tests: [0-9]* passed, 0 failed" $$L || { echo "FAIL: disk self-tests"; ok=0; }; \
	  grep -q "fault)> hi" $$L || { echo "FAIL: keyboard echo"; ok=0; }; \
	  grep -q "Memory: [0-9]* MiB usable, paging on" $$L || { echo "FAIL: memory init"; ok=0; }; \
	  grep -q "initramfs: [0-9]* KiB at physical" $$L || { echo "FAIL: initramfs loaded"; ok=0; }; \
	  grep -q "Context switch: ping pong ping pong ping pong" $$L || { echo "FAIL: context switch"; ok=0; }; \
	  grep -q "Preemption: spin0 counted [1-9][0-9]*, spin1 counted [1-9]" $$L || { echo "FAIL: preemption"; ok=0; }; \
	  grep -q "brakes ran in slots: 4 9 14 19[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (brakes)"; ok=0; }; \
	  grep -q "main ran in slots: 1 10[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (main)"; ok=0; }; \
	  grep -q "Hello from user mode (ring 3), via int 0x80!" $$L || { echo "FAIL: user write()"; ok=0; }; \
	  grep -q "args one two" $$L || { echo "FAIL: argv"; ok=0; }; \
	  grep -q "User mode: fpu exited with code 0" $$L || { echo "FAIL: FPU state"; ok=0; }; \
	  grep -q "files: all [0-9]* checks passed" $$L || { echo "FAIL: step 7 files"; ok=0; }; \
	  grep -q "User mode: hello exited with code 42" $$L || { echo "FAIL: user exit()"; ok=0; }; \
	  grep -q "peek: killed by CPU exception 14" $$L || { echo "FAIL: user page fault"; ok=0; }; \
	  grep -q "Kernel read of address 00000000 (page not mapped)" $$L || { echo "FAIL: NULL page fault"; ok=0; }; \
	  if [ $$ok = 1 ]; then echo "TEST OK"; else echo "TEST FAILED"; exit 1; fi

# Boot the benchmark build; it prints its results and switches QEMU off
# through the isa-debug-exit device (QEMU then exits with status 1 — fine).
bench:
	@$(MAKE) --no-print-directory MODE=bench run-bench

run-bench: $(IMG) $(USB)
	@$(QEMU) $(QEMU_FLAGS) -display none -serial file:$(BUILD)/serial.log \
	  -icount shift=0,sleep=off -device isa-debug-exit,iobase=0xf4,iosize=0x04 || true
	@sed -n '/^CPU:/p;/^Cycle counter/p;/^Boot:/p;/^Benchmarks/,/^Benchmarks done/p' $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)
