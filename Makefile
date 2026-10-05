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
#   make uefi       build/myos-uefi.img: GPT + FAT16 ESP, boots on UEFI (no CSM)
#   ARCH=i386       default: Pentium kernel + Alpine x86 userland
#   ARCH=x86_64     64-bit kernel (long mode hello). make / make test-uefi
#                   Userland fetch: python3 tools/fetch-live.py --arch x86_64
#   make run-uefi-text  boot that image in QEMU+OVMF, serial only
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

# One tree, two chips. ARCH picks the CPU dir, the toolchain, and Alpine.
ARCH ?= i386
ifeq ($(filter $(ARCH),i386 x86_64),)
  $(error ARCH must be i386 or x86_64 (got $(ARCH)))
endif
ifeq ($(ARCH),i386)
  ALPINE_ARCH := x86
else
  ALPINE_ARCH := x86_64
endif

ifeq ($(ARCH),x86_64)
  CC      := x86_64-elf-gcc
  OBJCOPY := x86_64-elf-objcopy
  LD      := x86_64-elf-ld
  NM      := x86_64-elf-nm
else
  CC      := i686-elf-gcc
  OBJCOPY := i686-elf-objcopy
  LD      := i686-elf-ld
  NM      := i686-elf-nm
endif
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
ifeq ($(ARCH),x86_64)
  ifeq ($(MODE),test)
    BUILD := build/x86_64/test
  else ifeq ($(MODE),bench)
    BUILD := build/x86_64/bench
  else
    BUILD := build/x86_64
  endif
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
           -DARCH_I386 -Iinclude -Iarch/i386/include $(MODE_FLAGS)
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
ifeq ($(ARCH),x86_64)
ALPINE := build/ext/alpine64
ALPINE_STAMP := $(ALPINE)/.fetched64
endif
OBJS    := $(BUILD)/arch/i386/entry.o $(ASM_OBJS) $(C_OBJS) $(GEN_OBJS)
LINKER_SCRIPT := arch/i386/linker.ld
NASM_FMT := elf32

# x86_64: hello = UEFI smoke test (early.c). full = portable kernel (PORT-PLAN).
#   make ARCH=x86_64 test-uefi          # hello (default)
#   make ARCH=x86_64 X86_64_KERNEL=full # port build (needs asm twins)
X86_64_KERNEL ?= full
ifeq ($(ARCH),x86_64)
  CFLAGS  := -std=c11 -ffreestanding -O2 -Wall -Wextra -g \
             -m64 -mno-red-zone -mcmodel=small -mgeneral-regs-only \
             -fno-tree-loop-distribute-patterns -DARCH_X86_64 \
             -Iinclude -Iarch/x86_64/include $(MODE_FLAGS)
  LDFLAGS := -T arch/x86_64/linker.ld -nostdlib -ffreestanding -O2 \
             -Wl,--no-warn-rwx-segments
  LINKER_SCRIPT := arch/x86_64/linker.ld
  NASM_FMT := elf64
  ifeq ($(X86_64_KERNEL),full)
    C_SRCS  := $(filter-out $(TEST_SRCS) $(BENCH_SRCS) arch/x86_64/early.c kernel/elf.c kernel/signal.c, \
               $(wildcard arch/x86_64/*.c kernel/*.c drivers/*.c fs/*.c mm/*.c net/*.c \
                          drivers/gpu/drm/*.c drivers/gpu/drm/tiny/*.c drivers/video/*.c))
    ifeq ($(MODE),test)
      C_SRCS += $(TEST_SRCS)
    endif
    ifeq ($(MODE),bench)
      C_SRCS += $(BENCH_SRCS)
    endif
    C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
    ASM_SRCS := $(filter-out arch/x86_64/entry.asm,$(wildcard arch/x86_64/*.asm))
    ASM_OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))
    OBJS    := $(BUILD)/arch/x86_64/entry.o $(ASM_OBJS) $(C_OBJS) $(GEN_OBJS)
  else
    C_SRCS  := arch/x86_64/early.c
    C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
    OBJS    := $(BUILD)/arch/x86_64/entry.o $(C_OBJS)
  endif
endif

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

.PHONY: diskattacks verify-disktests verify-tests attacks all dist utm utm-hd hd live-root live-extra live-theme live-hd utm-live run-live run-hd test-hd run run-full run-text debug test check-selftests test-shell bench run-bench usb clean fuzz fuzz-build fuzz-replay diskfuzz diskfuzz-replay verify-racetests uefi run-uefi-text test-uefi live-uefi test-live-uefi live-login-initrd test-uefi-login

ifeq ($(ARCH),x86_64)
all: uefi
else
all: $(IMG) $(USB)
endif

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
	$(NASM) -f $(NASM_FMT) $< -o $@

-include $(C_OBJS:.o=.d)

# --- time-slot table ---------------------------------------------------------
# The kernel can't read files yet, so config/slots.conf becomes a C array.
# Write to .tmp first so a config error doesn't leave a half-written file.
$(BUILD)/gen/slots_conf.c: config/slots.conf tools/slots2c.awk
	@mkdir -p $(dir $@)
	awk -f tools/slots2c.awk $< > $@.tmp && mv $@.tmp $@

$(BUILD)/gen/slots_conf.o: $(BUILD)/gen/slots_conf.c include/sched.h include/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) $(LINKER_SCRIPT)
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
ifneq ($(ARCH),x86_64)
$(ALPINE_STAMP): tools/fetch-alpine.sh
	tools/fetch-alpine.sh $(ALPINE)
endif

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

ifeq ($(ARCH),x86_64)
$(ALPINE_STAMP):
	tools/fetch-alpine64.sh $(ALPINE)

$(INITRD): $(INITRD_FILES) $(ALPINE_STAMP) tools/busybox-applets.txt Makefile
	@rm -rf $(BUILD)/initramfs
	@mkdir -p $(BUILD)/initramfs/bin $(BUILD)/initramfs/dev $(BUILD)/initramfs/proc \
	          $(BUILD)/initramfs/tmp $(BUILD)/initramfs/etc
	cp -R initramfs/. $(BUILD)/initramfs/
	cp $(ALPINE)/bin/busybox.static $(BUILD)/initramfs/bin/busybox
	cd $(BUILD)/initramfs/bin && for a in $$(cat $(CURDIR)/tools/busybox-applets.txt); do \
	  [ -e "$$a" ] || ln -sf busybox "$$a"; done
	ln -sf bin $(BUILD)/initramfs/sbin
	chmod 1777 $(BUILD)/initramfs/tmp
	cd $(BUILD)/initramfs && find . -mindepth 1 ! -name .DS_Store | sort \
	  | cpio -o -H newc -R 0:0 --quiet > ../initrd.cpio
else
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
endif

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

# --- UEFI disk (x64 firmware starts BOOTX64.EFI; we then enter the 32-bit kernel)
# The EFI app is 64-bit. Apple clang can compile Windows/PE objects; we have
# no lld, so tools/coff2efi.py links them. OVMF comes with Homebrew's qemu.
UEFI_IMG   := $(BUILD)/myos-uefi.img
OVMF_CODE  := /opt/homebrew/share/qemu/edk2-x86_64-code.fd
OVMF_VARS  := /opt/homebrew/share/qemu/edk2-i386-vars.fd
QEMU64     := qemu-system-x86_64
CLANG_EFI  := clang -target x86_64-unknown-windows -std=c11 -ffreestanding \
              -fno-builtin -fno-stack-protector -fno-stack-check \
              -fno-asynchronous-unwind-tables -fno-unwind-tables -fshort-wchar \
              -mno-red-zone -mno-stack-arg-probe -mno-mmx -mno-sse -mno-sse2 \
              -fno-pic -fno-pie -O2 -g0 -Wall -Wextra

$(BUILD)/efi/efi_jump.bin: arch/i386/boot/efi/efi_jump.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(BUILD)/efi/efi_jump.inc: $(BUILD)/efi/efi_jump.bin
	python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); print(",".join("0x%02x"%b for b in d))' $< > $@

ifeq ($(ARCH),x86_64)
$(BUILD)/efi/efi_main.o: arch/x86_64/boot/efi/efi_main.c arch/i386/boot/efi/efi.h \
                         arch/x86_64/include/bootinfo.h $(BUILD)/kernel.elf
	@mkdir -p $(dir $@)
	END=0x$$($(NM) $(BUILD)/kernel.elf | awk '$$3 == "__kernel_end" { print $$1 }'); \
	LOAD=$$(( (END + 4095) / 4096 * 4096 )); \
	$(CLANG_EFI) -DKERNEL_LOAD=0x100000 -DKERNEL_END=$$END -DINITRD_LOAD=$$LOAD \
	  -Iinclude -Iarch/i386/boot/efi -Iarch/x86_64/include -c $< -o $@

$(UEFI_IMG): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(INITRD) \
             arch/i386/boot/efi/vga-font.bin tools/mkuefi.py
	python3 tools/mkuefi.py --out $@ --esp-mb 64 \
	  --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	  --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	  --file EFI/MYOS/INITRD.CPIO=$(INITRD) \
	  --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
else
$(BUILD)/efi/efi_main.o: arch/i386/boot/efi/efi_main.c $(BUILD)/efi/efi_jump.inc \
                         arch/i386/boot/efi/efi.h $(BUILD)/kernel.elf
	@mkdir -p $(dir $@)
	END=0x$$($(NM) $(BUILD)/kernel.elf | awk '$$3 == "__kernel_end" { print $$1 }'); \
	LOAD=$$(( (END - 0xC0000000 + 4095) / 4096 * 4096 )); \
	$(CLANG_EFI) -DINITRD_LOAD=$$LOAD -I$(BUILD)/efi -Iarch/i386/boot/efi \
	  -c $< -o $@

$(UEFI_IMG): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(INITRD) \
             arch/i386/boot/efi/vga-font.bin tools/mkuefi.py
	python3 tools/mkuefi.py --out $@ --esp-mb 64 \
	  --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	  --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	  --file EFI/MYOS/INITRD.CPIO=$(INITRD) \
	  --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
endif

$(BUILD)/BOOTX64.EFI: $(BUILD)/efi/efi_main.o tools/coff2efi.py
	python3 tools/coff2efi.py -o $@ $<

uefi: $(UEFI_IMG)

# OVMF is 64-bit firmware, so this is qemu-system-x86_64 (not i386 / pentium3).
# Serial is the console we can see with -display none.
QEMU_UEFI_FLAGS = -machine q35 -m 256M -cpu qemu64 \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	-drive if=none,format=raw,file=$(UEFI_IMG),id=bootdisk \
	-device ide-hd,drive=bootdisk,bootindex=0 \
	-net none -no-reboot

run-uefi-text: $(UEFI_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	$(QEMU64) $(QEMU_UEFI_FLAGS) -display none -serial stdio

test-uefi: $(UEFI_IMG)
	@rm -f $(BUILD)/uefi-serial.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 22; echo quit) \
	  | $(QEMU64) $(QEMU_UEFI_FLAGS) -display none \
	    -serial file:$(BUILD)/uefi-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/uefi-serial.log; echo
	@grep -q "Hello world from MyOS" $(BUILD)/uefi-serial.log \
	  || { echo "FAIL: UEFI did not start the kernel"; exit 1; }
ifeq ($(ARCH),i386)
	@grep -q "initramfs:" $(BUILD)/uefi-serial.log \
	  || { echo "FAIL: UEFI kernel ran but initramfs missing"; exit 1; }
	@echo "UEFI OK: OVMF started the 32-bit kernel"
else
	@grep -q "long mode" $(BUILD)/uefi-serial.log \
	  || { echo "FAIL: 64-bit kernel did not say long mode"; exit 1; }
	@echo "UEFI OK: OVMF started the 64-bit kernel"
endif

# Login-only initramfs (rules sit with live-hd: they need LOGIN_BIN).
# x86_64 paths match tools/fetch-live.py --arch x86_64 (not build/x86_64/live).
ifeq ($(ARCH),x86_64)
  LIVE_SYSROOT      := build/live/sysroot-x86_64
  LIVE_EXTRA        := build/live/extra.x86_64
  LIVE_TAR          := build/live/rootfs-x86_64.tar
  LIVE_LOGIN_TAR    := build/live/login-root-x86_64.tar
  LIVE_LOGIN_INITRD := build/live/login-initrd-x86_64.cpio
  LIVE_LOCK         := live/packages.x86_64.lock
  LIVE_HD           := $(BUILD)/myos-live-root.img
  LIVE_MB           := 2048
  MUSL_LDSO         := /lib/ld-musl-x86_64.so.1
  LIVE_MARCH        := -march=x86-64
  ALPINE_TRIPLET    := x86_64-alpine-linux-musl
  MKUEFI_ROOT       := --root-file $(LIVE_HD)
else
  LIVE_SYSROOT      := $(BUILD)/live/sysroot
  LIVE_EXTRA        := $(BUILD)/live/extra
  LIVE_TAR          := $(BUILD)/live/rootfs.tar
  LIVE_LOGIN_TAR    := $(BUILD)/live/login-root.tar
  LIVE_LOGIN_INITRD := $(BUILD)/live/login-initrd.cpio
  LIVE_LOCK         := live/packages.lock
  LIVE_HD           := $(BUILD)/myos-live.img
  LIVE_MB           := 1536
  MUSL_LDSO         := /lib/ld-musl-i386.so.1
  LIVE_MARCH        := -march=i686
  ALPINE_TRIPLET    := i586-alpine-linux-musl
  MKUEFI_ROOT       := --root-file $(LIVE_HD) --root-from-mbr
endif

# Live desktop on the same GPT disk: ESP + ext2 "myos-root" from make live-hd.
# The ESP carries the login initrd so a machine with no visible disks still
# reaches myos-login from RAM. QEMU still mounts the ext2 partition.
# The 64-bit kernel is still hello-only: it will not start this initrd yet.
LIVE_UEFI := $(BUILD)/myos-live-uefi.img
$(LIVE_UEFI): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(LIVE_LOGIN_INITRD) $(LIVE_HD) \
              arch/i386/boot/efi/vga-font.bin tools/mkuefi.py
	ESP=$$(( $$(wc -c < $(LIVE_LOGIN_INITRD)) / 1048576 + 32 )); \
	  [ $$ESP -lt 128 ] && ESP=128; \
	  python3 tools/mkuefi.py --out $@ --esp-mb $$ESP $(MKUEFI_ROOT) \
	    --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	    --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	    --file EFI/MYOS/INITRD.CPIO=$(LIVE_LOGIN_INITRD) \
	    --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
live-uefi: $(LIVE_UEFI)

test-live-uefi: $(LIVE_UEFI)
	@rm -f $(BUILD)/live-uefi-serial.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 50; echo quit) \
	  | $(QEMU64) -machine q35 -m 1024M -cpu qemu64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	    -drive if=none,format=raw,file=$(LIVE_UEFI),id=bootdisk \
	    -device ide-hd,drive=bootdisk,bootindex=0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/live-uefi-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/live-uefi-serial.log; echo
	@grep -q "Hello world from MyOS" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: live UEFI did not start the kernel"; exit 1; }
	@grep -q "type 83 Linux" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: GPT Linux partition not seen"; exit 1; }
	@grep -q "mounted at /" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: myos-root did not become /"; exit 1; }
	@echo "LIVE UEFI OK: OVMF started the kernel and mounted myos-root"

# RAM-only login (no ext2): what a real UEFI laptop does until USB MSC exists.
UEFI_LOGIN_IMG := $(BUILD)/myos-uefi-login.img
$(UEFI_LOGIN_IMG): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(LIVE_LOGIN_INITRD) \
                   arch/i386/boot/efi/vga-font.bin tools/mkuefi.py
	ESP=$$(( $$(wc -c < $(LIVE_LOGIN_INITRD)) / 1048576 + 32 )); \
	  [ $$ESP -lt 128 ] && ESP=128; \
	  python3 tools/mkuefi.py --out $@ --esp-mb $$ESP \
	    --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	    --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	    --file EFI/MYOS/INITRD.CPIO=$(LIVE_LOGIN_INITRD) \
	    --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
test-uefi-login: $(UEFI_LOGIN_IMG)
	@rm -f $(BUILD)/uefi-login-serial.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 70; echo quit) \
	  | $(QEMU64) -machine q35 -m 1024M -cpu qemu64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	    -drive if=none,format=raw,file=$(UEFI_LOGIN_IMG),id=bootdisk \
	    -device ide-hd,drive=bootdisk,bootindex=0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/uefi-login-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/uefi-login-serial.log; echo
	@grep -q "Hello world from MyOS" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: login UEFI did not start the kernel"; exit 1; }
	@grep -q "initramfs: [1-9][0-9][0-9][0-9] files" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: login initrd did not unpack"; exit 1; }
	@grep -qE "MyOS is up|First boot:|/init exited|myos-login" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: runit/myos-login did not start"; exit 1; }
	@echo "UEFI LOGIN OK: RAM userland reached runit or myos-login"

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
live-root: live-extra
	python3 tools/fetch-live.py --arch $(ALPINE_ARCH)

# What MyOS itself puts on the live disk, on top of the packages
# (build/live/extra, copied in by tools/fetch-live.py):
#   the Golden Hour theme    IceWM's theme and the login screen's pictures,
#                            drawn from the palette in assets/themes
#   myos-login               the graphical login screen (live/myos-login)
# myos-login is a Linux program, not kernel code: it is compiled against
# Alpine's own headers and libraries, unpacked into build/live/sysroot by
# `tools/fetch-live.py --sysroot`, and linked the way every program on the
# disk is -- dynamically, against musl's loader. (PAM needs that: it opens
# its modules at run time.)
LOGIN_BIN    := $(LIVE_EXTRA)/usr/bin/myos-login
GCC_INC      := $(shell $(CC) -print-file-name=include)
LOGIN_CFLAGS := -std=c11 -O2 -Wall -Wextra $(LIVE_MARCH) -D_GNU_SOURCE -nostdinc \
                -isystem $(LIVE_SYSROOT)/usr/include \
                -isystem $(LIVE_SYSROOT)/usr/include/freetype2 -isystem $(GCC_INC)
LOGIN_LDLIBS := -lX11 -lXft -lImlib2 -lpam -lc -lgcc

$(LIVE_SYSROOT)/.done:
	python3 tools/fetch-live.py --arch $(ALPINE_ARCH) --sysroot

$(LOGIN_BIN): live/myos-login/myos-login.c live/common/xpic.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-login.o
	$(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(BUILD)/live/myos-login.o -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  $(LOGIN_LDLIBS) $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@

# The Start menu (live/myos-menu): the same libraries, less PAM.
MENU_BIN := $(LIVE_EXTRA)/usr/bin/myos-menu
$(MENU_BIN): live/myos-menu/myos-menu.c live/common/xpic.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-menu.o
	$(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(BUILD)/live/myos-menu.o -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  -lX11 -lXft -lImlib2 -lc -lgcc $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@

# Settings (live/myos-settings): a GTK3 program, and myos-xsettings, which
# applies the desktop's scale. GTK's headers come from packages.txt's
# [build-only] group; its libraries are on the disk already.
SETTINGS_BIN  := $(LIVE_EXTRA)/usr/bin/myos-settings
XSETTINGS_BIN := $(LIVE_EXTRA)/usr/bin/myos-xsettings
GTK_CFLAGS := $(addprefix -isystem $(LIVE_SYSROOT)/usr/include/,gtk-3.0 glib-2.0 pango-1.0 cairo \
                gdk-pixbuf-2.0 atk-1.0 harfbuzz webkitgtk-4.1 javascriptcoregtk-4.1 libsoup-3.0) \
                -isystem $(LIVE_SYSROOT)/usr/lib/glib-2.0/include
LIVE_LINK = $(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(1) -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  $(2) -lc -lgcc $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@
$(SETTINGS_BIN): live/myos-settings/myos-settings.c live/myos-settings/scale.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations -c $< -o $(BUILD)/live/myos-settings.o
	$(call LIVE_LINK,$(BUILD)/live/myos-settings.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lcairo -lX11 -lm)
# My Files (live/myos-files): GTK3 for the window, cairo and pango for
# the drawing, librsvg for the theme's SVG icons.
FILES_BIN := $(LIVE_EXTRA)/usr/bin/myos-files
$(FILES_BIN): live/myos-files/myos-files.c live/myos-settings/scale.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -isystem $(LIVE_SYSROOT)/usr/include/librsvg-2.0 \
	  -isystem $(LIVE_SYSROOT)/usr/include/poppler/glib \
	  -Ilive/myos-settings -Wno-deprecated-declarations -Wno-format-truncation \
	  -c $< -o $(BUILD)/live/myos-files.o
	$(call LIVE_LINK,$(BUILD)/live/myos-files.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lpoppler-glib \
	  -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lrsvg-2 -lX11 -lm)
# Preview (live/myos-preview): the Mac's Preview — PDFs through poppler
# (qpdf, run as a program, rewrites their pages), pictures through
# gdk-pixbuf's loaders, and TIFF, JPEG 2000, OpenEXR, Photoshop and HEIC
# through their libraries. /usr/bin/preview is the short name.
PREVIEW_BIN  := $(LIVE_EXTRA)/usr/bin/myos-preview
PREVIEW_SRC  := $(wildcard live/myos-preview/*.c)
PREVIEW_OBJS := $(PREVIEW_SRC:live/myos-preview/%.c=$(BUILD)/live/preview/%.o)
$(BUILD)/live/preview/%.o: live/myos-preview/%.c live/myos-preview/preview.h live/myos-settings/scale.h \
                           | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) $(addprefix -isystem $(LIVE_SYSROOT)/usr/include/,poppler/glib \
	  openjpeg-2.5) -Wno-deprecated-declarations -c $< -o $@
$(PREVIEW_BIN): $(PREVIEW_OBJS) live/myos-preview/preview.desktop
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/usr/share/applications
	$(call LIVE_LINK,$(PREVIEW_OBJS),-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lpoppler-glib -lgio-2.0 \
	  -lgobject-2.0 -lglib-2.0 -lpangocairo-1.0 -lpango-1.0 -lcairo -ltiff -lopenjp2 -lheif \
	  -lOpenEXR-3_1 -lX11 -lm)
	ln -sf myos-preview $(dir $@)preview
	install -m 644 live/myos-preview/preview.desktop $(LIVE_EXTRA)/usr/share/applications/
$(XSETTINGS_BIN): live/myos-settings/myos-xsettings.c live/myos-settings/scale.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-xsettings.o
	$(call LIVE_LINK,$(BUILD)/live/myos-xsettings.o,-lX11)
# Settings' "Add User…": checks an administrator's password with PAM and
# writes the account files. Setuid root, as passwd is.
ADDUSER_BIN := $(LIVE_EXTRA)/usr/libexec/myos-adduser
$(ADDUSER_BIN): live/myos-settings/myos-adduser.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-adduser.o
	$(call LIVE_LINK,$(BUILD)/live/myos-adduser.o,-lpam)
	chmod 4755 $@

# WritePerfect: Rodger's WordPerfect 5.1 work-alike, a separate project
# built from its own tree (WP_DIR) into /usr/lib/writeperfect, with its
# grammar checker (languagetool-native, C++) and the checker's English
# rules. /usr/bin/wp (live/writeperfect/wp) starts it.
#
# The C++ is compiled by clang, not i686-elf-g++: the bare-metal target
# makes size_t `unsigned long`, Linux i386 `unsigned int`, and C++ puts the
# type in every symbol's name (operator new is _Znwm there, _Znwj in
# Alpine's libstdc++), so nothing would link. clang targets Linux exactly;
# GNU ld from i686-elf links, with Alpine's own crtbegin/crtend and libgcc.
WP_DIR   ?= ../wp51/wp51c
WP_LT    := $(WP_DIR)/../languagetool-native
WP_OUT   := $(BUILD)/live/wp
WP_BIN   := $(LIVE_EXTRA)/usr/lib/writeperfect/wp
WP_GCC   := $(firstword $(wildcard $(LIVE_SYSROOT)/usr/lib/gcc/$(ALPINE_TRIPLET)/*))
WP_CXXV  := $(notdir $(firstword $(wildcard $(LIVE_SYSROOT)/usr/include/c++/*)))
LIVE_CLANG := clang --target=$(ALPINE_TRIPLET) $(LIVE_MARCH) --sysroot=$(abspath $(LIVE_SYSROOT)) -O2 -MMD -MP
# agent.c calls select() without <sys/select.h>; glibc brings it in through
# other headers, musl doesn't.
WP_CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -w -include sys/select.h -I$(WP_DIR)/src -I$(WP_LT)/engine/include $(GTK_CFLAGS)
WP_CXXFLAGS := -std=c++17 -w -nostdinc++ -isystem $(LIVE_SYSROOT)/usr/include/c++/$(WP_CXXV) \
               -isystem $(LIVE_SYSROOT)/usr/include/c++/$(WP_CXXV)/$(ALPINE_TRIPLET) \
               -isystem $(LIVE_SYSROOT)/usr/include/libxml2 -I$(WP_LT)/engine/include \
               -I$(WP_LT)/morfologik/include -DPCRE2_CODE_UNIT_WIDTH=8 \
               '-DLT_RESOURCE_DIR="/usr/share/writeperfect/languagetool"'
WP_C   := doc wpd view cmd files screen iofmt font agent grammar clip extra extra_cmd zh_ai main \
          gui_gtk hanzi_stroke plugin_host
WP_CPP := utf16 StringTools Regex ResourceDataBroker core_types Rule Language JLanguageTool \
          WordTokenizer EnglishWordTokenizer SRXSentenceTokenizer morfologik EnglishTagger \
          XmlRuleDisambiguator EnglishChunker EnglishSynthesizer RuleFilters rules_builtin \
          rules_en PatternRuleLoader lt_c_api
WP_OBJS := $(WP_C:%=$(WP_OUT)/%.o) $(WP_CPP:%=$(WP_OUT)/lt/%.o)
-include $(WP_OBJS:.o=.d)

$(WP_OUT)/%.o: $(WP_DIR)/src/%.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CFLAGS) -c $< -o $@
$(WP_OUT)/%.o: $(WP_DIR)/linux/%.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CFLAGS) -c $< -o $@
$(WP_OUT)/lt/%.o: $(WP_LT)/engine/src/%.cpp | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CXXFLAGS) -c $< -o $@

$(WP_BIN): $(WP_OBJS) live/writeperfect/wp live/writeperfect/writeperfect \
           live/writeperfect/writeperfect.desktop
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/usr/bin $(LIVE_EXTRA)/usr/share/applications
	$(CC) -nostdlib -s -Wl,-z,noexecstack -Wl,--eh-frame-hdr -Wl,--as-needed \
	  -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o $(WP_GCC)/crtbegin.o \
	  $(WP_OBJS) -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib -L$(WP_GCC) \
	  -lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lgdk_pixbuf-2.0 \
	  $(if $(wildcard $(LIVE_SYSROOT)/usr/lib/libwebkit2gtk-4.1.so*),-lwebkit2gtk-4.1 -ljavascriptcoregtk-4.1,) \
	  -lncursesw -lz -lxml2 -lpcre2-8 -licuuc -licui18n -lstdc++ -lm -lc -lgcc -lgcc_s \
	  $(WP_GCC)/crtend.o $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@
	install -m 755 live/writeperfect/wp live/writeperfect/writeperfect $(LIVE_EXTRA)/usr/bin/
	install -m 644 live/writeperfect/writeperfect.desktop $(LIVE_EXTRA)/usr/share/applications/
	@mkdir -p $(LIVE_EXTRA)/usr/share/writeperfect
	rsync -a --delete --exclude '*.class' --exclude '*.java' \
	  $(WP_LT)/resources/ $(LIVE_EXTRA)/usr/share/writeperfect/languagetool/
	# Help and samples: the GTK window's Help menu and Sample docs, as on the Mac.
	rsync -a --delete --exclude 'build_*' --exclude '*.c' \
	  $(WP_DIR)/help/ $(LIVE_EXTRA)/usr/share/writeperfect/help/
	rsync -a --delete --exclude 'build_*' --exclude '*.c' \
	  $(WP_DIR)/samples/ $(LIVE_EXTRA)/usr/share/writeperfect/samples/
	# HTML reveal plugins: ZHwriter, Thesaurus, Writer, Reveal Codes.
	rsync -a --delete --exclude '*.LICENSE' \
	  $(WP_DIR)/plugins/ $(LIVE_EXTRA)/usr/share/writeperfect/plugins/

# Built only where WP_DIR is: MyOS's own source on the live disk has no
# WritePerfect tree, and the rest of the disk builds without it.
ifeq ($(wildcard $(WP_DIR)/src/main.c),)
WP_BIN :=
endif

LIVE_THEME := $(LIVE_EXTRA)/.theme
$(LIVE_THEME): live/theme/make-live-theme.py assets/themes/golden-hour/make-theme.py \
               live/writeperfect/writeperfect.svg live/myos-preview/preview.svg
	LIVE_EXTRA=$(LIVE_EXTRA) python3 live/theme/make-live-theme.py
	@touch $@

live-theme: $(LIVE_THEME)
live-extra: $(LIVE_THEME) $(LOGIN_BIN) $(MENU_BIN) $(SETTINGS_BIN) $(XSETTINGS_BIN) $(ADDUSER_BIN) $(FILES_BIN) $(PREVIEW_BIN) $(WP_BIN)

# The live disk: like `make hd`, but myos-root holds the live userland
# (tools/mklivehd.sh: owners, setuid bits and case-only names kept).
$(LIVE_TAR): $(LIVE_THEME) $(LOGIN_BIN) $(MENU_BIN) $(SETTINGS_BIN) $(XSETTINGS_BIN) $(ADDUSER_BIN) $(FILES_BIN) $(PREVIEW_BIN) $(WP_BIN) $(LIVE_LOCK) $(shell find live/overlay -type f)
	python3 tools/fetch-live.py --arch $(ALPINE_ARCH)
ifeq ($(ARCH),x86_64)
$(LIVE_HD): $(LIVE_TAR) tools/mklivehd.sh
	E2FS=$(E2FS) tools/mklivehd.sh - $(LIVE_TAR) $@ $(LIVE_MB)
else
$(LIVE_HD): $(IMG) $(LIVE_TAR) tools/mklivehd.sh tools/mbr-partition.sh
	E2FS=$(E2FS) tools/mklivehd.sh $(IMG) $(LIVE_TAR) $@ $(LIVE_MB)
endif
live-hd: $(LIVE_HD)

# RAM login for a UEFI stick when the kernel cannot see the ext2 root.
# The Pentium floppy initrd stays BusyBox. WritePerfect is left on disk.
$(LIVE_LOGIN_TAR): $(LIVE_THEME) $(LOGIN_BIN) $(MENU_BIN) $(XSETTINGS_BIN) \
                   $(LIVE_LOCK) tools/fetch-live.py \
                   $(shell find live/overlay -type f)
	python3 tools/fetch-live.py --arch $(ALPINE_ARCH) --login-root
	@tar -tf $@ | grep -q '^usr/bin/myos-login$$' \
	  || { echo "login-root.tar has no myos-login"; exit 1; }
$(LIVE_LOGIN_INITRD): $(LIVE_LOGIN_TAR) tools/tar2cpio.py live/ram-init
	python3 tools/tar2cpio.py $< $@ --add init=live/ram-init
	@test $$(wc -c < $@) -lt $$((384 * 1024 * 1024)) \
	  || { echo "login initrd is over 384 MiB"; exit 1; }
live-login-initrd: $(LIVE_LOGIN_INITRD)

# The live disk in UTM, as its own machine (its own UUID).
UTM_LIVE_VM := dist/MyOS live.utm
utm-live: $(LIVE_HD)
	@mkdir -p dist
	tools/mkutm.sh --installed --replace-disk --memory 1024 $(LIVE_HD) "$(UTM_LIVE_VM)" 5C81D2E7-0B4F-4A93-8E6D-71F3A9C2B540

run-live: $(LIVE_HD)
	$(QEMU) -m 1024M -cpu pentium3,+sse2 -drive format=raw,file=$(LIVE_HD),index=0,media=disk \
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
ifeq ($(ARCH),x86_64)
test: test-uefi
else
test:
	@$(MAKE) --no-print-directory MODE=test check-selftests
endif

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
	  grep -q "shmtest: all ok" $$L || { echo "FAIL: System V shared memory (MIT-SHM)"; ok=0; }; \
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
	@$(MAKE) -s dropbox-ignore

# This tree lives in Dropbox, and build/ is 1.6 GB of object files and disk
# images that `make` regenerates. Syncing it wastes the account's space and
# the link's bandwidth, so it is marked with Dropbox's own ignore attribute --
# which keeps the files here and removes them from the cloud copy.
#
# The attribute lives on the directory, so `make clean` destroys it and the
# sync starts over. Hence re-applying it here, and after clean. Silent and
# never fatal: the attribute only exists on macOS, and a missing directory or
# a machine without Dropbox is not an error.
.PHONY: dropbox-ignore
dropbox-ignore:
	@for d in $(BUILD) dist disks; do \
	  [ -d "$$d" ] && xattr -w com.dropbox.ignored 1 "$$d" 2>/dev/null; \
	done; true
