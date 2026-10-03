# =============================================================================
# MyOS Makefile
#
#   make            build build/myos.img (bootable raw disk image)
#   make run        boot in QEMU: VGA in a window (the text is scaled to fill the
#                   window — make the window bigger for bigger text), serial
#                   output in this terminal
#   make run-full   the same, full screen with the text scaled up to fill it
#                   (leave full screen: Cmd-F, or the View menu)
#   make run-text   boot in QEMU fully in the terminal (VGA text screen via curses;
#                   serial goes to build/serial.log). Quit: Esc then 2, type "quit".
#   make debug      like run, but paused at the first instruction; then in another
#                   terminal: gdb build/kernel.elf -ex "target remote localhost:1234"
#   make test       build with self-tests (build/test/), boot headless, print the
#                   serial log, fail unless every check passes
#   make bench      build with benchmarks (build/bench/), boot headless, print cycles
#   make test-shell boot the normal image, type commands into the BusyBox shell,
#                   check the output
#   make usb        (re)build build/usb.img, the FAT32 test disk (needs mtools)
#   make dist       dist/myos-usb.zip: the boot image + instructions, to write to a
#                   USB stick or run in an emulator on another computer
#   make utm        "dist/MyOS read-write.utm": a ready-made UTM virtual machine
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
#   tools/mksplash.py          --png-->   build/splash.bin  (320x200, mode 13h)
#   myos.img = [ mbr.bin | stage2.bin | splash.bin | kernel.bin | initrd.cpio | padding ]
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
# 4 KiB reserved for stage 2 on the disk (layout.inc gets it with -D).
# The boot picture is 320x200 bytes, one per pixel: 125 sectors exactly.
STAGE2_SECTORS := 8
SPLASH_SECTORS := 125
USB     := build/usb.img

# The data disk: an ext2 file system the kernel mounts at /data (fs/ext2.c),
# where files survive a restart. `make run` & co. use disks/data.img, made
# once and then kept — your files live there; `make clean` leaves it alone
# (delete it yourself for a fresh, empty one). Tests and benchmarks get a
# fresh disk of their own every run. Tools: Homebrew's e2fsprogs.
E2FS    := /opt/homebrew/opt/e2fsprogs/sbin
MKE2FS  := $(E2FS)/mke2fs
E2FSCK  := $(E2FS)/e2fsck
DEBUGFS := $(E2FS)/debugfs
# Plain ext2 (no journal, no hash index), owned by root.
MKE2FS_BASE := -q -t ext2 -O none,filetype,sparse_super
MKE2FS_OPTS := $(MKE2FS_BASE) -E root_owner=0:0 -L myos-data
DATA    := disks/data.img
ifneq ($(MODE),normal)
  DATA  := $(BUILD)/data.img
endif

# -cpu pentium3: the features both target CPUs (Pentium 4, Pentium M) have —
# so we never rely on anything the old machines lack.
# (Recursive "=": a target may use a disk of its own, see test-shell.)
QEMU_FLAGS = -m 64M -cpu pentium3 \
             -drive format=raw,file=$(IMG),index=0,media=disk \
             -drive format=raw,file=$(USB),index=1,media=disk \
             -drive format=raw,file=$(DATA),index=2,media=disk

.PHONY: all dist utm utm-hd hd run-hd test-hd run run-full run-text debug test check-selftests test-shell bench run-bench usb clean

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

# --- Alpine Linux programs ---------------------------------------------------
# Unmodified 32-bit x86 Linux programs from Alpine (tools/fetch-alpine.sh:
# pinned versions + checksums, downloaded once):
#   BusyBox  static: a shell and ~300 Unix commands in one program
#   bash     dynamically linked: needs musl's dynamic linker and C library
#            (/lib/ld-musl-i386.so.1), readline and ncurses (/usr/lib), and
#            terminal descriptions (/etc/terminfo)
ALPINE := build/ext/alpine
ALPINE_STAMP := $(ALPINE)/.fetched

$(ALPINE_STAMP): tools/fetch-alpine.sh
	tools/fetch-alpine.sh $(ALPINE)

# --- initramfs ---------------------------------------------------------------
# The root file system, packed into a cpio archive ("newc" format, the one
# Linux uses), owned by root:
#   initramfs/         /init, /etc/...
#   BusyBox            /bin/busybox, plus a link /bin/NAME -> busybox for each
#                      of its commands (tools/busybox-applets.txt);
#                      /sbin, /usr/bin, /usr/sbin are links to /bin
#   bash               /bin/bash, /lib (musl), /usr/lib (readline, ncurses),
#                      /etc/terminfo (linux, vt100, dumb), /etc/inputrc
# Test and bench builds add the test programs as /bin/NAME, the files in
# user/rootfs/, and data/numbers.txt (~110 KB of test data).
INITRD_FILES := $(shell find initramfs -type f 2>/dev/null)
TEST_FILES   := $(shell find user/rootfs -type f 2>/dev/null)

$(INITRD): $(INITRD_FILES) $(ALPINE_STAMP) tools/busybox-applets.txt \
           $(if $(MODE_FLAGS),$(USER_BINS) $(TEST_FILES)) Makefile
	@rm -rf $(BUILD)/initramfs
	@mkdir -p $(BUILD)/initramfs/bin $(BUILD)/initramfs/usr/lib $(BUILD)/initramfs/root \
	          $(BUILD)/initramfs/tmp $(BUILD)/initramfs/lib $(BUILD)/initramfs/etc/terminfo
	cp -R initramfs/. $(BUILD)/initramfs/
ifneq ($(MODE_FLAGS),)
	@mkdir -p $(BUILD)/initramfs/test
	for f in $(USER_BINS); do cp $$f $(BUILD)/initramfs/bin/$$(basename $$f .elf); done
	cp -R user/rootfs/. $(BUILD)/initramfs/
	seq 1 20000 > $(BUILD)/initramfs/test/numbers.txt
endif
	cp $(ALPINE)/bin/busybox.static $(BUILD)/initramfs/bin/busybox
	cp $(ALPINE)/bin/bash $(BUILD)/initramfs/bin/bash
	cp -P $(ALPINE)/lib/ld-musl-i386.so.1 $(ALPINE)/lib/libc.musl-x86.so.1 $(BUILD)/initramfs/lib/
	cp -P $(ALPINE)/usr/lib/libreadline.so.* $(ALPINE)/usr/lib/libncursesw.so.* \
	      $(BUILD)/initramfs/usr/lib/
	for t in l/linux v/vt100 d/dumb; do mkdir -p $(BUILD)/initramfs/etc/terminfo/$$(dirname $$t); \
	  cp $(ALPINE)/etc/terminfo/$$t $(BUILD)/initramfs/etc/terminfo/$$t; done
	cp $(ALPINE)/etc/inputrc $(BUILD)/initramfs/etc/inputrc
	cd $(BUILD)/initramfs/bin && for a in $$(cat $(CURDIR)/tools/busybox-applets.txt); do \
	  [ -e "$$a" ] || [ -L "$$a" ] || ln -s busybox "$$a"; done
	ln -s bin $(BUILD)/initramfs/sbin
	ln -s ../bin $(BUILD)/initramfs/usr/bin
	ln -s ../bin $(BUILD)/initramfs/usr/sbin
	chmod 1777 $(BUILD)/initramfs/tmp
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
	  -DSPLASH_LBA=$$((1 + $(STAGE2_SECTORS))) -DSPLASH_SECTORS=$(SPLASH_SECTORS) \
	  -DKERNEL_LBA=$$((1 + $(STAGE2_SECTORS) + $(SPLASH_SECTORS))) -DKERNEL_SECTORS=$$K \
	  -DINITRD_LBA=$$((1 + $(STAGE2_SECTORS) + $(SPLASH_SECTORS) + K)) -DINITRD_SECTORS=$$I \
	  -DINITRD_BYTES=$$(( $$(wc -c < $(INITRD)) )) \
	  -DINITRD_LOAD=$$(( (END - 0xC0000000 + 4095) / 4096 * 4096 )) \
	  $< -o $@

# --- disk image --------------------------------------------------------------
# 1.44 MB of zeros (it grows if the pieces need more), then the pieces one
# after another. "conv=notrunc" overwrites without shrinking the file;
# "sync" pads the last partial sector with zeros.
$(BUILD)/splash.bin: assets/myos-logo.png tools/mksplash.py
	@mkdir -p $(dir $@)
	python3 tools/mksplash.py $< $@
	@test $$(wc -c < $@) -eq 64000

$(IMG): $(BUILD)/mbr.bin $(BUILD)/stage2.bin $(BUILD)/splash.bin $(BUILD)/kernel.bin $(INITRD) tools/mbr-partition.sh
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=$(BUILD)/mbr.bin    of=$@ bs=512 seek=0 conv=notrunc 2>/dev/null
	dd if=$(BUILD)/stage2.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=$(BUILD)/splash.bin of=$@ bs=512 seek=$$((1 + $(STAGE2_SECTORS))) \
	  conv=notrunc 2>/dev/null
	dd if=$(BUILD)/kernel.bin of=$@ bs=512 seek=$$((1 + $(STAGE2_SECTORS) + $(SPLASH_SECTORS))) \
	  conv=notrunc,sync 2>/dev/null
	K=$(call sectors,$(BUILD)/kernel.bin); \
	dd if=$(INITRD) of=$@ bs=512 seek=$$((1 + $(STAGE2_SECTORS) + $(SPLASH_SECTORS) + K)) \
	  conv=notrunc,sync 2>/dev/null
	tools/mbr-partition.sh $@
	@echo "Built $@ (kernel: $$(wc -c < $(BUILD)/kernel.bin | tr -d ' ') bytes," \
	  "initramfs: $$(wc -c < $(INITRD) | tr -d ' ') bytes)"

# --- a package to share ------------------------------------------------------
# The normal boot image is a complete disk: written byte for byte to a USB
# stick (dd, Rufus...), a PC's BIOS boots it. The README explains how.
dist: $(IMG)
	@mkdir -p dist
	cp $(IMG) dist/myos-usb.img
	cp tools/dist-README.txt dist/README.txt
	cd dist && rm -f myos-usb.zip && zip -q myos-usb.zip myos-usb.img README.txt
	@echo "dist/myos-usb.zip: $$(wc -c < dist/myos-usb.zip | tr -d ' ') bytes"

# The same disk inside a ready-made UTM virtual machine (macOS): double-click
# it in Finder. tools/mkutm.sh explains the machine settings.
UTM_VM := dist/MyOS read-write.utm
utm: $(IMG)
	@mkdir -p dist
	tools/mkutm.sh $(IMG) "$(UTM_VM)"

# An installed MyOS in UTM: one disk with the whole system (make hd). Its
# disk holds your files, so rebuilding keeps it.
UTM_HD_VM := dist/MyOS installed.utm
utm-hd: $(HD)
	@mkdir -p dist
	tools/mkutm.sh --installed $(HD) "$(UTM_HD_VM)"

# --- FAT32 test disk ---------------------------------------------------------
$(USB): tools/mkusb.sh
	tools/mkusb.sh $@

usb:
	tools/mkusb.sh $(USB)

# --- an installed MyOS: the whole system on the disk (step 9c) -----------------
#   myos-hd.img = [ the boot area, as in myos.img | ext2 "myos-root", ROOT_MB ]
# The kernel finds the partition labelled myos-root and mounts it as "/";
# everything (programs, /etc, /root, /tmp) is then on the disk and survives
# a restart. The installer (to come) makes the same layout on a real disk.
# The file system is filled from the same files as the initramfs (mke2fs -d).
HD      := $(BUILD)/myos-hd.img
ROOT_MB := 256
$(HD): $(IMG) tools/mbr-partition.sh
	@rm -rf $(BUILD)/rootfs; cp -R $(BUILD)/initramfs $(BUILD)/rootfs
	@mkdir -p $(BUILD)/rootfs/dev $(BUILD)/rootfs/data   # mount points
	cp $(IMG) $@
	S=$$(( $$(wc -c < $(IMG)) / 512 )); START=$$(( (S + 2047) / 2048 * 2048 )); \
	  $(MKE2FS) $(MKE2FS_BASE) -L myos-root -b 4096 -d $(BUILD)/rootfs \
	    -E offset=$$((START * 512)),root_owner=0:0 -F $@ $(ROOT_MB)M && \
	  tools/mbr-partition.sh $@ $$S $$START $$(( $(ROOT_MB) * 2048 ))
	@echo "Built $@ (boot area + $(ROOT_MB) MiB ext2 myos-root)"

hd: $(HD)

run-hd: $(HD) $(USB)
	$(QEMU) -m 64M -cpu pentium3 -drive format=raw,file=$(HD),index=0,media=disk \
	  -drive format=raw,file=$(USB),index=1,media=disk $(DISPLAY_OPTS) -serial stdio

# Boot the installed disk twice: the first boot changes files all over the
# tree, the second must see them. Then Linux's e2fsck checks the root
# partition and debugfs reads a file MyOS wrote.
HD_TEST := $(BUILD)/test-hd.img
define boot-hd-typing
	@rm -f $(BUILD)/serial.log
	@(sleep 10; for c in $(1); do \
	    tools/qemu-type.sh "$$c" | while read k; do echo "$$k"; sleep 0.04; done; sleep 2; \
	  done; sleep 1; echo quit) \
	  | $(QEMU) -m 64M -cpu pentium3 -drive format=raw,file=$(HD_TEST),index=0,media=disk \
	    -display none -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/serial.log >> $(BUILD)/serial-hd.txt
endef
test-hd: $(HD)
	@cp $(HD) $(HD_TEST); rm -f $(BUILD)/serial-hd.txt
	$(call boot-hd-typing,'echo kept > /root/notes.txt; mkdir /work; echo hi >> /etc/hostname; sync')
	$(call boot-hd-typing,'cat /root/notes.txt; ls -d /work; tail -n 1 /etc/hostname')
	@sed -n '/mounted at \//p;/^myos:/,$$p' $(BUILD)/serial-hd.txt | grep -v '^$$'
	@L=$(BUILD)/serial-hd.txt; ok=1; \
	  [ $$(grep -c "ext2: hda2 mounted at /:" $$L) = 2 ] || { echo "FAIL: root not mounted from hda2"; ok=0; }; \
	  grep -q "^kept" $$L || { echo "FAIL: /root/notes.txt lost"; ok=0; }; \
	  grep -q "^/work" $$L || { echo "FAIL: /work lost"; ok=0; }; \
	  grep -q "^hi" $$L || { echo "FAIL: /etc change lost"; ok=0; }; \
	  [ $$ok = 1 ] || { echo "HD TEST FAILED"; exit 1; }
	@S=$$(( $$(wc -c < $(IMG)) / 512 )); START=$$(( (S + 2047) / 2048 * 2048 )); \
	  dd if=$(HD_TEST) of=$(BUILD)/test-root.img bs=512 skip=$$START count=$$(( $(ROOT_MB) * 2048 )) 2>/dev/null
	$(call check-test-disk,$(BUILD)/test-root.img,/root/notes.txt,kept)
	@echo "HD TEST OK"

# --- the ext2 data disk -------------------------------------------------------
# 64 MiB with 4 KiB blocks. Made only if it isn't there.
disks/data.img:
	@mkdir -p disks
	$(MKE2FS) $(MKE2FS_OPTS) -b 4096 -F $@ 64M

# A fresh, empty one for a benchmark build.
$(BUILD)/data.img:
	@mkdir -p $(dir $@)
	$(MKE2FS) $(MKE2FS_OPTS) -b 4096 -F $@ 16M

# The test disk: 1 KiB blocks (so files soon need indirect blocks), made by
# Linux's mke2fs from a directory of known files; user/ext2test.c reads them.
TEST_E2ROOT := $(BUILD)/e2root
define make-test-disk
	@rm -rf $(TEST_E2ROOT) $(1); mkdir -p $(TEST_E2ROOT)/dir/sub
	@echo "hello from ext2" > $(TEST_E2ROOT)/hello.txt
	@echo nested > $(TEST_E2ROOT)/dir/sub/nested.txt
	@ln -s hello.txt $(TEST_E2ROOT)/link
	@seq 1 50000 > $(TEST_E2ROOT)/numbers.txt
	@$(MKE2FS) $(MKE2FS_OPTS) -b $(2) -d $(TEST_E2ROOT) -F $(1) 16M
endef

# After a test boot: is the disk still a valid ext2 file system (Linux's
# e2fsck), and does Linux's debugfs read what MyOS wrote ($(2) = $(3))?
define check-test-disk
	@$(E2FSCK) -fn $(1) > $(1).fsck 2>&1 \
	  || { echo "FAIL: e2fsck found damage on the disk MyOS wrote:"; cat $(1).fsck; exit 1; }
	@[ "$$($(DEBUGFS) -R 'cat $(2)' $(1) 2>/dev/null)" = "$(3)" ] \
	  || { echo "FAIL: debugfs can't read $(2) back from the disk"; exit 1; }
	@echo "DISK OK: e2fsck clean, debugfs reads $(2)"
endef

# --- running -----------------------------------------------------------------
# The PC text screen is 720x400 pixels; zoom-to-fit scales it up to the
# window (or the whole screen), so the letters grow with it.
DISPLAY_OPTS := -display cocoa,zoom-to-fit=on

run: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS) -serial stdio

run-full: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS),full-screen=on -serial stdio

run-text: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) -display curses -serial file:$(BUILD)/serial.log

debug: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS) -serial stdio -s -S

# Boot with no display and drive it through QEMU's monitor: wait for boot,
# "type" h, i, Enter, press F10 (NULL pointer -> page fault), then quit.
# The kernel's serial output is the test result; each grep checks one thing.
test:
	@$(MAKE) --no-print-directory MODE=test check-selftests

check-selftests: $(IMG) $(USB)
	$(call make-test-disk,$(DATA),1024)
	@rm -f $(BUILD)/serial.log
	@(sleep 4; for k in h i ret; do echo "sendkey $$k"; sleep 0.2; done; \
	  echo "mouse_move 10 5"; sleep 0.3; echo "mouse_button 1"; sleep 0.3; \
	  echo "mouse_button 0"; sleep 0.3; \
	  echo "sendkey f10"; sleep 1; echo quit) \
	  | $(QEMU) $(QEMU_FLAGS) -display none \
	    -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@cat $(BUILD)/serial.log; echo
	@L=$(BUILD)/serial.log; ok=1; \
	  grep -q "Timer: 1000 Hz (sleep 100 ms = 10[0-9] ticks)" $$L || { echo "FAIL: timer"; ok=0; }; \
	  grep -q "Self-tests: [0-9]* passed, 0 failed" $$L || { echo "FAIL: disk self-tests"; ok=0; }; \
	  grep -q "fault)> hi" $$L || { echo "FAIL: keyboard echo"; ok=0; }; \
	  grep -q "mouse: PS/2, with a wheel" $$L || { echo "FAIL: PS/2 mouse found"; ok=0; }; \
	  grep -q "mouse: dx=10 dy=-5 wheel=0 buttons=0" $$L || { echo "FAIL: mouse movement"; ok=0; }; \
	  grep -q "mouse: dx=0 dy=0 wheel=0 buttons=1" $$L || { echo "FAIL: mouse click"; ok=0; }; \
	  grep -q "Memory: [0-9]* MiB usable, paging on" $$L || { echo "FAIL: memory init"; ok=0; }; \
	  grep -q "initramfs: [0-9]* KiB at physical" $$L || { echo "FAIL: initramfs loaded"; ok=0; }; \
	  grep -q "Context switch: ping pong ping pong ping pong" $$L || { echo "FAIL: context switch"; ok=0; }; \
	  grep -q "Preemption: spin0 counted [1-9][0-9]*, spin1 counted [1-9]" $$L || { echo "FAIL: preemption"; ok=0; }; \
	  grep -q "brakes ran in slots: 4 9 14 19[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (brakes)"; ok=0; }; \
	  grep -q "main ran in slots: 1 10[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (main)"; ok=0; }; \
	  grep -q "Hello from user mode (ring 3), via int 0x80!" $$L || { echo "FAIL: user write()"; ok=0; }; \
	  grep -q "args one two" $$L || { echo "FAIL: argv"; ok=0; }; \
	  grep -q "User mode: fpu exited with code 0" $$L || { echo "FAIL: FPU state"; ok=0; }; \
	  grep -q "systest: all [0-9]* checks passed" $$L || { echo "FAIL: step 8 startup calls"; ok=0; }; \
	  grep -q "fstest /fst: all [0-9]* checks passed" $$L || { echo "FAIL: step 8i writable files"; ok=0; }; \
	  grep -q "pcache: second read comes from RAM" $$L || { echo "FAIL: step 9c-2 page cache"; ok=0; }; \
	  grep -q "pcache: read-ahead filled the next page" $$L || { echo "FAIL: step 9c-2 read-ahead"; ok=0; }; \
	  grep -q "fatvfs: HELLO.TXT read through the VFS" $$L || { echo "FAIL: step 9e FAT32 in the VFS"; ok=0; }; \
	  grep -q "ext2test: all [0-9]* checks passed" $$L || { echo "FAIL: step 9b ext2 (Linux-made files)"; ok=0; }; \
	  grep -q "fstest /data/fst: all [0-9]* checks passed" $$L || { echo "FAIL: step 9b ext2 (fstest)"; ok=0; }; \
	  grep -q "sigtest: all [0-9]* checks passed" $$L || { echo "FAIL: step 8 signals"; ok=0; }; \
	  grep -q "ttytest: all [0-9]* checks passed" $$L || { echo "FAIL: step 8 terminal"; ok=0; }; \
	  grep -q "pipetest: all [0-9]* checks passed" $$L || { echo "FAIL: step 8 pipes"; ok=0; }; \
	  grep -q "forktest: all [0-9]* checks passed" $$L || { echo "FAIL: step 8 processes"; ok=0; }; \
	  grep -q "files: all [0-9]* checks passed" $$L || { echo "FAIL: step 7 files"; ok=0; }; \
	  grep -q "User mode: hello exited with code 42" $$L || { echo "FAIL: user exit()"; ok=0; }; \
	  grep -q "peek: killed by CPU exception 14" $$L || { echo "FAIL: user page fault"; ok=0; }; \
	  grep -q "Kernel read of address 00000000 (page not mapped)" $$L || { echo "FAIL: NULL page fault"; ok=0; }; \
	  [ $$ok = 1 ] || { echo "TEST FAILED"; exit 1; }
	$(call check-test-disk,$(DATA),/persist.txt,written by MyOS)
	@echo "TEST OK"

# Boot the normal image (/init starts bash) and type commands at it through
# QEMU's monitor (tools/qemu-type.sh); check what the shell printed (with
# the terminal escape sequences taken out).
SHELL_TEST_CMDS := 'echo hello from $$0 $$BASH_VERSION' 'ls /' 'echo one two three | wc -w' \
                   'cat /etc/hostname' 'cd /etc; pwd' 'uname -sm' \
                   'cd /tmp; mkdir -p a/b; echo saved > a/b/f; cp a/b/f a/g; mv a/g a/h; echo more >> a/h; echo lines=$$(wc -l < a/h)' \
                   'sed -i s/saved/edited/ a/h; head -n 1 a/h' 'rm -r a && echo removed $$(ls -A | wc -l)' \
                   'cd /data; mkdir -p a/b; echo kept > a/b/f; cp /bin/busybox bb; mv bb a/; rm numbers.txt; sync' \
                   'cat /data/a/b/f; cmp /bin/busybox /data/a/bb && echo same busybox' \
                   'for i in 1 2 3; do echo -n "n$$i "; done; echo' 'sh -c "echo busybox sh too"'

test-shell: DATA := $(BUILD)/shell-data.img
test-shell: $(IMG) $(USB)
	$(call make-test-disk,$(DATA),4096)
	@rm -f $(BUILD)/serial.log
	@(sleep 5; for c in $(SHELL_TEST_CMDS); do \
	    tools/qemu-type.sh "$$c" | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  done; \
	  echo "sendkey up"; sleep 0.2; echo "sendkey ret"; sleep 1; \
	  tools/qemu-type.sh "sleep 30" | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  echo "sendkey ctrl-c"; sleep 1; \
	  tools/qemu-type.sh "echo after ctrl-c" | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 1; echo quit) \
	  | $(QEMU) $(QEMU_FLAGS) -display none -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/serial.log > $(BUILD)/serial.txt
	@sed -n '/Welcome/,$$p' $(BUILD)/serial.txt; echo
	@L=$(BUILD)/serial.txt; ok=1; \
	  grep -q "^hello from /bin/bash 5\.2" $$L || { echo "FAIL: bash"; ok=0; }; \
	  grep -q "README.txt" $$L || { echo "FAIL: ls"; ok=0; }; \
	  grep -q "^3[[:space:]]*$$" $$L || { echo "FAIL: echo | wc -w"; ok=0; }; \
	  grep -q "^myos[[:space:]]*$$" $$L || { echo "FAIL: cat"; ok=0; }; \
	  grep -q "^/etc[[:space:]]*$$" $$L || { echo "FAIL: cd/pwd"; ok=0; }; \
	  grep -q "^Linux i686" $$L || { echo "FAIL: uname"; ok=0; }; \
	  grep -q "^lines=2" $$L || { echo "FAIL: mkdir, >, >>, cp, mv"; ok=0; }; \
	  grep -q "^edited" $$L || { echo "FAIL: sed -i (temp file + rename)"; ok=0; }; \
	  grep -q "^removed 0" $$L || { echo "FAIL: rm -r"; ok=0; }; \
	  grep -q "^n1 n2 n3" $$L || { echo "FAIL: bash for loop"; ok=0; }; \
	  grep -q "^busybox sh too" $$L || { echo "FAIL: sh from bash"; ok=0; }; \
	  [ $$(grep -c "^busybox sh too" $$L) = 2 ] || { echo "FAIL: history (up arrow)"; ok=0; }; \
	  grep -q "^after ctrl-c" $$L || { echo "FAIL: Ctrl-C"; ok=0; }; \
	  grep -q "^kept" $$L || { echo "FAIL: files on the ext2 disk"; ok=0; }; \
	  grep -q "^same busybox" $$L || { echo "FAIL: a 1 MB copy on the ext2 disk"; ok=0; }; \
	  [ $$ok = 1 ] || { echo "SHELL TEST FAILED"; exit 1; }
	$(call check-test-disk,$(DATA),/a/b/f,kept)
	@echo "SHELL TEST OK"

# Boot the benchmark build; it prints its results and switches QEMU off
# through the isa-debug-exit device (QEMU then exits with status 1 — fine).
bench:
	@$(MAKE) --no-print-directory MODE=bench run-bench

run-bench: $(IMG) $(USB) $(DATA)
	@$(QEMU) $(QEMU_FLAGS) -display none -serial file:$(BUILD)/serial.log \
	  -icount shift=0,sleep=off -device isa-debug-exit,iobase=0xf4,iosize=0x04 || true
	@sed -n '/^CPU:/p;/^Cycle counter/p;/^Boot:/p;/^Benchmarks/,/^Benchmarks done/p' $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)
