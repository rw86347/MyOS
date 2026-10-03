# =============================================================================
# MyOS Makefile
#
#   make            build build/myos.img (bootable raw disk image)
#   make run        boot in QEMU: a 1920x1200 pixel console in a window (scaled
#                   to fit the window), serial output in this terminal
#   make run-full   the same, full screen
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
#   make live-root  build/live/rootfs.tar: all the live disk's software
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
             $(wildcard arch/i386/*.c kernel/*.c drivers/*.c fs/*.c mm/*.c net/*.c \
                        drivers/gpu/drm/*.c drivers/gpu/drm/tiny/*.c drivers/video/*.c))
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
             $(patsubst user/%.c,$(BUILD)/user/%.elf,$(wildcard user/*.c)) \
             $(patsubst user/musl/%.c,$(BUILD)/user/musl/%.elf,$(wildcard user/musl/*.c))
USER_LIB  := $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/mini.o
# Alpine Linux packages, unpacked (tools/fetch-alpine.sh; see the initramfs).
ALPINE := build/ext/alpine
ALPINE_STAMP := $(ALPINE)/.fetched
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
             -drive format=raw,file=$(DATA),index=2,media=disk \
             -netdev user,id=net0 -device e1000,netdev=net0

.PHONY: diskattacks verify-disktests verify-tests attacks all dist utm utm-hd hd live-root live-hd utm-live run-live run-hd test-hd run run-full run-text debug test check-selftests test-shell bench run-bench usb clean fuzz fuzz-build fuzz-replay diskfuzz diskfuzz-replay verify-racetests

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

# user/musl/NAME.c: linked statically against Alpine's real musl C library
# (musl-dev, tools/fetch-alpine.sh) — for what the mini libc can't do, like
# pthreads. (Before the user/%.c rule, which would match these too.)
MUSL_CFLAGS := -std=c11 -O2 -Wall -Wextra -march=i686 -nostdinc -isystem $(ALPINE)/usr/include
MUSL_LIB    := $(ALPINE)/usr/lib

$(BUILD)/user/musl/%.elf: user/musl/%.c | $(ALPINE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $(BUILD)/user/musl/$*.o
	$(CC) -nostdlib -static -Wl,-z,noexecstack $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o $(BUILD)/user/musl/$*.o \
	      -Wl,--start-group $(MUSL_LIB)/libc.a -lgcc -Wl,--end-group $(MUSL_LIB)/crtn.o -o $@

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
# (ALPINE and ALPINE_STAMP are set further up: the user programs use them.)
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

# --- the live disk's software (step 9c-3) ----------------------------------------
# Every program on the live disk (live/packages.txt: IceWM desktop, logins,
# AbiWord, gcc/make/python3/adb...), as pinned Alpine packages
# (live/packages.lock), plus the MyOS source, in one archive:
# build/live/rootfs.tar. `tools/fetch-live.py --update` re-resolves the list.
live-root:
	python3 tools/fetch-live.py

# The live disk: like `make hd`, but myos-root holds the live userland
# (tools/mklivehd.sh: owners, setuid bits and case-only names kept).
LIVE_TAR := $(BUILD)/live/rootfs.tar
LIVE_HD  := $(BUILD)/myos-live.img
LIVE_MB  := 1536
$(LIVE_TAR):
	python3 tools/fetch-live.py
$(LIVE_HD): $(IMG) $(LIVE_TAR) tools/mklivehd.sh tools/mbr-partition.sh
	E2FS=$(E2FS) tools/mklivehd.sh $(IMG) $(LIVE_TAR) $@ $(LIVE_MB)
live-hd: $(LIVE_HD)

# The live disk in UTM, as its own machine (its own UUID).
UTM_LIVE_VM := dist/MyOS live.utm
utm-live: $(LIVE_HD)
	@mkdir -p dist
	tools/mkutm.sh --installed $(LIVE_HD) "$(UTM_LIVE_VM)" 5C81D2E7-0B4F-4A93-8E6D-71F3A9C2B540

run-live: $(LIVE_HD)
	$(QEMU) -m 256M -cpu pentium3 -drive format=raw,file=$(LIVE_HD),index=0,media=disk \
	  $(DISPLAY_OPTS) -serial stdio

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
# QEMU's default card is the std VGA, so the Bochs driver switches to
# 1920x1200; zoom-to-fit scales that (or the text screen before it) to the window.
DISPLAY_OPTS := -display cocoa,zoom-to-fit=on

run: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS) -serial stdio

run-full: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS),full-screen=on -serial stdio

# curses can only show the text screen: a Cirrus card has no MyOS driver, so
# the console stays 80x25 text.
run-text: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) -vga cirrus -display curses -serial file:$(BUILD)/serial.log

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
	@(sleep 8; for k in h i ret; do echo "sendkey $$k"; sleep 0.2; done; \
	  echo "mouse_move 10 5"; sleep 0.3; echo "mouse_button 1"; sleep 0.3; \
	  echo "mouse_button 0"; sleep 0.3; \
	  echo "screendump $(BUILD)/screen.ppm"; sleep 0.5; \
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
	  grep -q "pthreadtest: all ok" $$L || { echo "FAIL: musl pthreads (clone threads, futex)"; ok=0; }; \
	  grep -q "racetest: all [0-9]* checks passed" $$L || { echo "FAIL: sleeping copy_to_user races"; ok=0; }; \
	  grep -q "boottest: all ok" $$L || { echo "FAIL: init/getty syscalls (mount, sethostname, reboot)"; ok=0; }; \
	  grep -q "pcache: second read comes from RAM" $$L || { echo "FAIL: step 9c-2 page cache"; ok=0; }; \
	  grep -q "pcache: read-ahead filled the next page" $$L || { echo "FAIL: step 9c-2 read-ahead"; ok=0; }; \
	  grep -q "fatvfs: HELLO.TXT read through the VFS" $$L || { echo "FAIL: step 9e FAT32 in the VFS"; ok=0; }; \
	  grep -q "net: loopback delivered" $$L || { echo "FAIL: step 10 loopback"; ok=0; }; \
	  grep -q "pci: e1000 8086:100e" $$L || { echo "FAIL: step 10 PCI e1000"; ok=0; }; \
	  grep -q "dhcp: lease from 10.0.2.2" $$L || { echo "FAIL: DHCP (the address alone proves nothing: the static fallback is the same one)"; ok=0; }; \
	  grep -q "ipv4: eth0 is 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3" $$L || { echo "FAIL: the lease was not applied"; ok=0; }; \
	  grep -q "net: ping 10.0.2.2 ok" $$L || { echo "FAIL: step 10 e1000 ping"; ok=0; }; \
	  grep -q "net: dns example.com is " $$L || { echo "FAIL: step 10 DNS"; ok=0; }; \
	  grep -q "net: tcp example.com HTTP ok" $$L || { echo "FAIL: step 10 TCP"; ok=0; }; \
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
	  grep -q "drm: bochs 1234:1111, 16 MiB, 1920x1200" $$L || { echo "FAIL: step 11b Bochs VGA driver"; ok=0; }; \
	  grep -q "fbcon: 120x37" $$L || { echo "FAIL: step 11b pixel console"; ok=0; }; \
	  grep -q "fbtest: all [0-9]* checks passed" $$L || { echo "FAIL: step 11b /dev/fb0"; ok=0; }; \
	  python3 tools/check-screen.py $(BUILD)/screen.ppm || { echo "FAIL: step 11b console on the screen"; ok=0; }; \
	  [ $$ok = 1 ] || { echo "TEST FAILED"; exit 1; }
	$(call check-test-disk,$(DATA),/persist.txt,written by MyOS)
	@$(DEBUGFS) -R 'dump /pattern.bin $(BUILD)/pattern.bin' $(DATA) >/dev/null 2>&1; \
	  python3 tools/check-pattern.py $(BUILD)/pattern.bin \
	  || { echo "FAIL: /pattern.bin on the disk isn't what ext2test wrote"; exit 1; }
	@echo "TEST OK"

# Boot the normal image (/init starts bash) and type commands at it through
# QEMU's monitor (tools/qemu-type.sh); check what the shell printed (with
# the terminal escape sequences taken out).
SHELL_TEST_CMDS := 'echo hello from $$0 $$BASH_VERSION' 'ls /' 'echo one two three | wc -w' \
                   'cat /etc/hostname' 'cd /etc; pwd' 'uname -sm' \
                   'ping -c 2 10.0.2.2' 'ping -c 1 -W 1 example.com' \
                   'cmp /proc/net/dns /etc/resolv.conf && echo resolv from kernel' \
                   'wget -q -O /tmp/e.html http://example.com/ && echo wget ok $$(wc -c < /tmp/e.html); rm -f /tmp/e.html' \
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
# Three of these checks are deliberately not anchored to the start of a line.
# The commands are typed on a timer, so a slow one -- ping waits a second
# between packets, wget does DNS and a fetch -- is still printing when the next
# one is typed into it, and its output ends up with "cd /t" in front. Anchoring
# those made the test fail at random. Unanchored is still exact here because
# each pattern contains something the echoed command line cannot: a received
# count, a resolved address, a byte count. The commands that return instantly
# keep their anchors.
	@L=$(BUILD)/serial.txt; ok=1; \
	  grep -q "^hello from /bin/bash 5\.2" $$L || { echo "FAIL: bash"; ok=0; }; \
	  grep -q "README.txt" $$L || { echo "FAIL: ls"; ok=0; }; \
	  grep -q "^3[[:space:]]*$$" $$L || { echo "FAIL: echo | wc -w"; ok=0; }; \
	  grep -q "^myos[[:space:]]*$$" $$L || { echo "FAIL: cat"; ok=0; }; \
	  grep -q "^/etc[[:space:]]*$$" $$L || { echo "FAIL: cd/pwd"; ok=0; }; \
	  grep -q "^Linux i686" $$L || { echo "FAIL: uname"; ok=0; }; \
	  grep -q "2 packets transmitted, 2 packets received" $$L || { echo "FAIL: ping 10.0.2.2 (raw ICMP socket, alarm)"; ok=0; }; \
	  grep -q "PING example.com ([0-9]" $$L || { echo "FAIL: DNS through musl (UDP socket, resolv.conf)"; ok=0; }; \
	  grep -q "^resolv from kernel$$" $$L || { echo "FAIL: /etc/resolv.conf was not taken from /proc/net/dns (so DNS only works where 10.0.2.3 exists)"; ok=0; }; \
	  grep -q "wget ok [1-9]" $$L || { echo "FAIL: wget (TCP socket)"; ok=0; }; \
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

# =============================================================================
# Fuzzing the packet parsers (tools/fuzz/)
# =============================================================================
# Builds net/*.c with the HOST compiler, not the cross compiler, and links it
# against tools/fuzz/stubs.c instead of the rest of the kernel. That is the
# whole point: on the host we get AddressSanitizer, so a one-byte overrun in a
# parser is an immediate, located failure instead of silent corruption that
# QEMU would happily run. The kernel sources are compiled unmodified, so a bug
# the fuzzer finds is a bug in the kernel.
#
#   make fuzz              fuzz every target for a while
#   make fuzz T=dns N=1e6  one target, longer
#   make fuzz-replay       re-run the saved corpus and crashes (for CI)
#
# Apple's clang has no libFuzzer runtime, so tools/fuzz/driver.c is the
# mutation loop; set FUZZER=libfuzzer where the real one exists.
FUZZ_CC    ?= clang
FUZZ_DIR   := build/fuzz
FUZZ_KSRCS := kernel/random.c net/skbuff.c net/ipv4.c net/udp.c net/tcp.c net/dns.c \
              net/dhcp.c \
              tools/fuzz/stubs.c tools/fuzz/packet.c tools/fuzz/targets.c
FUZZ_TARGETS := wire ip arp dns tcp dhcp
FUZZ_SAN   := -fsanitize=address,undefined -fno-sanitize-recover=all \
              -fsanitize-coverage=trace-pc-guard
FUZZ_WARN  := -std=c11 -g -O1 -Wall -Wextra -fno-omit-frame-pointer
# The kernel side compiles against the kernel's own headers, with
# -Itools/fuzz/shim FIRST so it replaces the two that are full of i386
# assembly (interrupts.h, uaccess.h) and nothing else.
FUZZ_KFLAGS := $(FUZZ_WARN) $(FUZZ_SAN) -Itools/fuzz/shim -Iinclude -Itools/fuzz
# The driver is ordinary host code and must NOT see -Iinclude: the kernel has
# its own string.h and errno.h, which would shadow the C library's and hide
# strerror, strncmp and errno.
FUZZ_DFLAGS := $(FUZZ_WARN) $(FUZZ_SAN) -Itools/fuzz

FUZZ_KOBJS := $(patsubst %.c,$(FUZZ_DIR)/%.o,$(FUZZ_KSRCS))
ifeq ($(FUZZER),libfuzzer)
  FUZZ_KFLAGS += -fsanitize=fuzzer
  FUZZ_OBJS   := $(FUZZ_KOBJS)
  FUZZ_LINK   := -fsanitize=fuzzer
else
  FUZZ_OBJS   := $(FUZZ_KOBJS) $(FUZZ_DIR)/tools/fuzz/driver.o
  FUZZ_LINK   :=
endif

$(FUZZ_DIR)/tools/fuzz/driver.o: tools/fuzz/driver.c tools/fuzz/targets.h
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_DFLAGS) -c -o $@ $<

$(FUZZ_DIR)/%.o: %.c $(wildcard tools/fuzz/*.h) $(wildcard tools/fuzz/shim/*.h)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_KFLAGS) -c -o $@ $<

$(FUZZ_DIR)/fuzz_net: $(FUZZ_OBJS)
	$(FUZZ_CC) $(FUZZ_SAN) $(FUZZ_LINK) -o $@ $^

fuzz-build: $(FUZZ_DIR)/fuzz_net

# Tests that assert an attack is rejected — the half fuzzing cannot reach,
# because accepting a forged ARP reply is not a memory error. Same stubs, same
# sanitizers, different question: not "did it survive" but "did it refuse".
# --- the disk side: the kernel's real filesystem code against a crafted
# image, under AddressSanitizer. The source list is almost the whole
# filesystem stack; only the allocators, the block device and the clock are
# stubbed (tools/fuzz/diskstubs.c), and the stubs are made more sensitive than
# the real thing so an overflow is a report rather than a quiet corruption.
# -Wno-macro-redefined: the kernel's signal.h and the host's both define NSIG.
DISK_KSRCS := fs/ext2.c fs/bcache.c fs/pcache.c fs/partition.c fs/fat32.c \
              fs/fatvfs.c fs/vfs.c fs/ramfs.c kernel/blockdev.c \
              tools/fuzz/diskstubs.c tools/fuzz/disktarget.c \
              tools/fuzz/diskattacks.c
DISK_OBJS  := $(patsubst %.c,$(FUZZ_DIR)/disk/%.o,$(DISK_KSRCS))
DISK_FLAGS := $(FUZZ_WARN) $(FUZZ_SAN) -Wno-macro-redefined \
              -Itools/fuzz/shim -Iinclude -Itools/fuzz

$(FUZZ_DIR)/disk/%.o: %.c $(wildcard tools/fuzz/*.h) $(wildcard tools/fuzz/shim/*.h)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(DISK_FLAGS) -c -o $@ $<

# A small, real ext2 image to corrupt: made by mke2fs so the cases start from
# something the kernel is meant to accept. 4 KiB blocks, which matters more
# than it looks: bcache gives every buffer a whole page, so with 1 KiB blocks
# a read 200 bytes past the end of a block is still inside the page and
# nothing notices. At 4 KiB the block fills the page exactly and the next byte
# is in the poisoned redzone. Six of the eight cases were silently measuring
# nothing until this changed.
$(FUZZ_DIR)/base.ext2:
	@mkdir -p $(FUZZ_DIR)/baseroot/sub/deep
	@echo "hello from the base image" > $(FUZZ_DIR)/baseroot/file.txt
	@awk 'BEGIN{for(i=0;i<500;i++)print "padding line to spill into a second block"}' \
	    > $(FUZZ_DIR)/baseroot/sub/big.txt
	@echo deep > $(FUZZ_DIR)/baseroot/sub/deep/leaf.txt
	@ln -sf file.txt $(FUZZ_DIR)/baseroot/link
	@ln -sf /a/long/path/that/is/not/there $(FUZZ_DIR)/baseroot/sub/longlink
	@rm -f $@
	$(MKE2FS) $(MKE2FS_BASE) -E root_owner=0:0 -L myos-data -b 4096 \
	    -N 256 -d $(FUZZ_DIR)/baseroot $@ 1024

$(FUZZ_DIR)/diskattacks: $(DISK_OBJS)
	$(FUZZ_CC) $(FUZZ_SAN) -o $@ $^

diskattacks: $(FUZZ_DIR)/diskattacks $(FUZZ_DIR)/base.ext2
	@$(FUZZ_DIR)/diskattacks $(FUZZ_DIR)/base.ext2

#   make diskfuzz              mutate the image and mount it, over and over
#   make diskfuzz DISK_N=1000000   longer
#
# The deterministic cases above are one person's list of what a hostile disk
# might say. This is the part that finds what nobody thought of: take the base
# image, change a few bytes near something structural, mount it, walk it,
# write to it. Each run is a fork, because the point is to survive a crash and
# report it rather than die. Coverage lives in shared memory so the parent can
# tell a new code path from a repeat and keep the input that found it.
#
# It found three bugs in its first minutes that reading the code had missed,
# two of which only matter on ARM (misaligned halfword access). See
# docs/security.md.
DISK_N ?= 100000
DISK_SEED ?= 1
diskfuzz: $(FUZZ_DIR)/diskattacks $(FUZZ_DIR)/base.ext2
	@mkdir -p $(FUZZ_DIR)/crashes tools/fuzz/corpus/disk
	@$(FUZZ_DIR)/diskattacks --fuzz $(DISK_N) $(DISK_SEED) $(FUZZ_DIR)/base.ext2

# Replay the kept corpus and every saved crash. Fast and deterministic: this
# is the one to run after touching fs/. Corpus entries are deltas against
# base.ext2, so the base image has to match -- it is generated, not committed,
# and the rule above is reproducible.
diskfuzz-replay: $(FUZZ_DIR)/diskattacks $(FUZZ_DIR)/base.ext2
	@set -e; n=0; for f in tools/fuzz/corpus/disk/* $(FUZZ_DIR)/crashes/disk-*; do \
	  [ -f "$$f" ] || continue; \
	  $(FUZZ_DIR)/diskattacks --replay "$$f" $(FUZZ_DIR)/base.ext2 >/dev/null; \
	  n=$$((n+1)); \
	done; echo "DISK REPLAY OK ($$n inputs)"

# Put each ext2 fix back the way it was and check the matching case notices.
verify-disktests:
	@tools/fuzz/verify-disktests.sh

ATTACK_SRCS := $(filter-out tools/fuzz/targets.c,$(FUZZ_KSRCS)) tools/fuzz/attacks.c
ATTACK_OBJS := $(patsubst %.c,$(FUZZ_DIR)/%.o,$(ATTACK_SRCS))

$(FUZZ_DIR)/attacks: $(ATTACK_OBJS)
	$(FUZZ_CC) $(FUZZ_SAN) -o $@ $^

attacks: $(FUZZ_DIR)/attacks
	@$(FUZZ_DIR)/attacks

# Does each racetest check fail against the bug it is meant to catch? Boots a
# test kernel per case, so slower than the host-side ones: about 90 seconds.
verify-racetests:
	@tools/fuzz/verify-racetests.sh

# Does each adversarial test actually fail against the bug it is meant to
# catch? Reintroduces one bug at a time and checks a test notices. A test that
# passes either way is measuring nothing, which has happened twice here.
verify-tests:
	@tools/fuzz/verify-tests.sh

T ?=
N ?= 200000
# Crashes from earlier runs are moved aside first, so what this reports is
# what THIS run found. Without that, a crash saved while the source was
# deliberately broken -- which is how the targets get checked for being able
# to see anything at all -- is reported again by the next honest run, and the
# next twenty minutes go on investigating a bug that was fixed before the run
# started. That happened twice. Kept rather than deleted: a real crash found
# yesterday is still worth having.
fuzz: $(FUZZ_DIR)/fuzz_net tools/fuzz/seeds
	@if ls $(FUZZ_DIR)/crashes/* >/dev/null 2>&1; then \
	  mkdir -p $(FUZZ_DIR)/crashes-old; mv $(FUZZ_DIR)/crashes/* $(FUZZ_DIR)/crashes-old/; \
	  echo "note: earlier crashes moved to $(FUZZ_DIR)/crashes-old/"; fi
	@set -e; for t in $(if $(T),$(T),$(FUZZ_TARGETS)); do \
	  $(FUZZ_DIR)/fuzz_net $$t -runs=$(N) -seed=1 \
	    -corpus=tools/fuzz/seeds/$$t -corpus=tools/fuzz/corpus/$$t \
	    -out=$(FUZZ_DIR); \
	done
	@if ls $(FUZZ_DIR)/crashes/* >/dev/null 2>&1; then \
	  echo "FUZZ FOUND SOMETHING: $(FUZZ_DIR)/crashes/"; ls -1 $(FUZZ_DIR)/crashes/; exit 1; \
	else echo "FUZZ OK (no crashes)"; fi

# What the fuzzing actually reached, as source lines rather than edge counts.
# A separate build with llvm's profiling instead of the sanitizers, replaying
# every seed and every input the fuzzer kept. This is the number worth putting
# in docs/security.md: "25 million runs" says nothing on its own, "every line
# of parse_a and skip_name was executed" does.
FUZZ_COVFLAGS := $(FUZZ_WARN) -fprofile-instr-generate -fcoverage-mapping
$(FUZZ_DIR)/fuzz_cov: $(FUZZ_KSRCS) tools/fuzz/driver.c $(wildcard tools/fuzz/*.h)
	@mkdir -p $(FUZZ_DIR)
	@for s in $(FUZZ_KSRCS); do \
	  o=$(FUZZ_DIR)/cov-$$(echo $$s | tr / _ | sed 's/\.c$$/.o/'); \
	  $(FUZZ_CC) $(FUZZ_COVFLAGS) -Itools/fuzz/shim -Iinclude -Itools/fuzz -c -o $$o $$s || exit 1; \
	done
	$(FUZZ_CC) $(FUZZ_COVFLAGS) -Itools/fuzz -c -o $(FUZZ_DIR)/cov-driver.o tools/fuzz/driver.c
	$(FUZZ_CC) $(FUZZ_COVFLAGS) -o $@ $(FUZZ_DIR)/cov-net_*.o \
	  $(FUZZ_DIR)/cov-tools_fuzz_*.o $(FUZZ_DIR)/cov-driver.o

fuzz-coverage: $(FUZZ_DIR)/fuzz_cov
	@rm -rf $(FUZZ_DIR)/prof && mkdir -p $(FUZZ_DIR)/prof
	@for t in $(FUZZ_TARGETS); do \
	  files=$$(ls tools/fuzz/seeds/$$t/* tools/fuzz/corpus/$$t/* \
	              $(FUZZ_DIR)/corpus-$$t/* 2>/dev/null || true); \
	  [ -z "$$files" ] && continue; \
	  LLVM_PROFILE_FILE=$(FUZZ_DIR)/prof/$$t.profraw \
	    $(FUZZ_DIR)/fuzz_cov $$t -replay $$files >/dev/null 2>&1 || true; \
	done
	@xcrun llvm-profdata merge -sparse $(FUZZ_DIR)/prof/*.profraw -o $(FUZZ_DIR)/prof/all.profdata
	@xcrun llvm-cov report $(FUZZ_DIR)/fuzz_cov \
	  -instr-profile=$(FUZZ_DIR)/prof/all.profdata $(FUZZ_KSRCS) \
	  | sed -n '1,2p;/^net\//p;/^TOTAL/p'
	@echo ""
	@echo "per-function, the parsers:"
	@xcrun llvm-cov report $(FUZZ_DIR)/fuzz_cov -show-functions \
	  -instr-profile=$(FUZZ_DIR)/prof/all.profdata net/dns.c net/ipv4.c net/tcp.c net/udp.c \
	  | awk 'NR<=2 || /parse_a|skip_name|name_expand|on_ip|on_arp|udp_input|tcp_input|encode_name|remember|lookup/'

# Replay everything already saved: fast, deterministic, and the thing to run
# after touching net/ — a fixed bug must stay fixed.
fuzz-replay: $(FUZZ_DIR)/fuzz_net
	@set -e; for t in $(FUZZ_TARGETS); do \
	  files=$$(ls tools/fuzz/seeds/$$t/* tools/fuzz/corpus/$$t/* \
	              $(FUZZ_DIR)/corpus-$$t/* $(FUZZ_DIR)/crashes/*-$$t-* 2>/dev/null || true); \
	  if [ -n "$$files" ]; then \
	    echo "--- $$t"; $(FUZZ_DIR)/fuzz_net $$t -replay $$files >/dev/null; \
	  fi; \
	done
	@echo "FUZZ REPLAY OK"

clean:
	rm -rf $(BUILD)
