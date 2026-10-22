# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
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
#                   Userland fetch: python3 live/tools/fetch-live.py --arch x86_64
#   ARCH=arm64      aarch64 hello on QEMU "virt" (roadmap 14; M1 bare metal is 15).
#                   Needs: brew install aarch64-elf-gcc
#                   make ARCH=arm64 run | make ARCH=arm64 test
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
ifeq ($(filter $(ARCH),i386 x86_64 arm64),)
  $(error ARCH must be i386, x86_64, or arm64 (got $(ARCH)))
endif
ifeq ($(ARCH),i386)
  ALPINE_ARCH := x86
  UNAME_M     := i686
else ifeq ($(ARCH),arm64)
  ALPINE_ARCH := aarch64
  UNAME_M     := aarch64
else
  ALPINE_ARCH := x86_64
  UNAME_M     := x86_64
endif

ifeq ($(ARCH),arm64)
  CC      := aarch64-elf-gcc
  OBJCOPY := aarch64-elf-objcopy
  LD      := aarch64-elf-ld
  NM      := aarch64-elf-nm
else ifeq ($(ARCH),x86_64)
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
# Building on MyOS itself, or on any x86-64 Linux: there is no x86_64-elf
# cross compiler there, but the system's own gcc and binutils make the same
# freestanding kernel, with a few flags turning off what a Linux
# distribution's gcc does by default for programs (position-independent
# code, stack-clash probes, _FORTIFY_SOURCE, CET marks, a build-id note).
# The 64-bit kernel only: the 32-bit and ARM64 ones need cross compilers.
ifeq ($(ARCH),x86_64)
  ifeq ($(shell command -v $(CC) 2>/dev/null),)
    ifeq ($(shell uname -sm),Linux x86_64)
      CC      := gcc
      OBJCOPY := objcopy
      LD      := ld
      NM      := nm
      NATIVE_CFLAGS  := -fno-pie -fno-pic -fno-stack-clash-protection -U_FORTIFY_SOURCE \
                        -fcf-protection=none
      NATIVE_LDFLAGS := -no-pie -Wl,--build-id=none
    endif
  endif
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
else ifeq ($(ARCH),arm64)
  ifeq ($(MODE),test)
    BUILD := build/arm64/test
  else
    BUILD := build/arm64
  endif
endif
# make TRACE=1 <target>: the kernel prints every program started and every
# exit, and its serial-only log lines, on the screen — for a real machine
# with no serial port. A build folder of its own: normal builds never have it.
ifeq ($(TRACE),1)
  MODE_FLAGS += -DTRACE_EXEC
  BUILD := $(BUILD)/trace
endif

# make STACKDEPTH=1 <target>: every kernel stack starts filled with a
# pattern, and every few seconds the kernel prints the deepest any thread
# has gone yet ("stack: ... has used N of its M bytes") — for sizing
# THREAD_STACK (kernel/thread.c, "kernel stacks"). A build folder of its own.
ifeq ($(STACKDEPTH),1)
  MODE_FLAGS += -DSTACK_DEPTH
  BUILD := $(BUILD)/stackdepth
endif

# make PTAUDIT=1 <target>: when a program faults, print its registers and
# the memory around them, and check every process's page tables against
# the page allocator (arch/x86_64/paging.c, vmm_audit): a frame mapped while
# free, or shared by more mappings than it has owners. Fault time only: it
# doesn't change the timing that leads to a crash. A build folder of its own.
ifeq ($(PTAUDIT),1)
  MODE_FLAGS += -DPT_AUDIT
  BUILD := $(BUILD)/ptaudit
endif

# make SYSTRACE=1 <target>: every thread keeps its last 32 system calls
# (number, arguments, result, time) and a user fault prints them, with the
# last few of every other thread in the process — what a crashing program
# asked the kernel for just before. A build folder of its own.
ifeq ($(SYSTRACE),1)
  MODE_FLAGS += -DSYS_TRACE
  BUILD := $(BUILD)/systrace
endif

# make MEMDEBUG=1 <target>: freed pages and freed kmalloc objects are
# filled with 0xDEADBEEF and checked when they are handed out again, so a
# write to memory after it was freed is reported (mm/pmm.c, mm/kheap.c).
# Slow. A build folder of its own.
ifeq ($(MEMDEBUG),1)
  MODE_FLAGS += -DMEM_DEBUG
  BUILD := $(BUILD)/memdebug
endif

# make CMDLINE=word <target>: the kernel's command line (/proc/cmdline),
# built in until a boot loader passes one. "nodesktop": the login service
# doesn't start X, so the text console shows everything (with TRACE=1, the
# kernel's own lines too) — for a real machine with no serial port. Its own
# build folder.
ifneq ($(CMDLINE),)
  MODE_FLAGS += -DMYOS_CMDLINE='"$(CMDLINE)"'
  BUILD := $(BUILD)/$(subst $(empty) $(empty),-,$(CMDLINE))
endif

# make IWLDEBUG=1 <target>: iwlwifi's own debug lines (the commands it sends
# the firmware, scans, joining...) in the kernel log, for bringing a card up
# on a real machine (drivers/net/wireless/intel/iwlwifi/myos.c). Its own
# build folder; a 256 KiB kernel log.
ifeq ($(IWLDEBUG),1)
  MODE_FLAGS += -DMYOS_IWLDEBUG
  BUILD := $(BUILD)/iwldebug
endif

# make WIFISIM=1 <target>: the kernel also has the simulated Wi-Fi radio of
# the self-tests (drivers/net/wireless/sim.c: two access points, one WPA2),
# registered at boot, so the real Wi-Fi programs (wpa_supplicant, the tray
# icon) can be tried on a whole system in QEMU. Test code: a build folder
# of its own, never a normal boot.
ifeq ($(WIFISIM),1)
  MODE_FLAGS += -DWIFI_SIM
  BUILD := $(BUILD)/wifisim
endif

# make SLEEPTEST=1 <target>: QEMU has no lid; tools/fakelid.py's ACPI table
# gives it one, and this kernel opens and shuts it when a CPU is added in
# QEMU's monitor (kernel/fakelid_test.c), for make test-sleep. Test code: a
# build folder of its own, never a normal boot.
ifeq ($(SLEEPTEST),1)
  MODE_FLAGS += -DSLEEP_TEST
  BUILD := $(BUILD)/sleeptest
endif

# Freestanding: no host OS, no C library. Only compiler-provided headers like
# <stdint.h> and <stddef.h> are available.
# -fno-tree-loop-distribute-patterns: stop GCC from turning our memcpy/memset
# loops into calls to memcpy/memset — inside memcpy itself that would recurse.
# -mgeneral-regs-only: the kernel never uses FPU/SSE registers (no floats, and
# no SSE tricks in copies), so kernel threads never need them saved (fpu.c).
# -march=i686: Pentium Pro instruction set (cmov etc.), which every target has.
# -fstack-protector-strong: functions with a local array (or a local whose
# address is taken) put a secret "canary" word below their return address
# and check it before returning; an overrun of the array changes it, and the
# kernel stops (__stack_chk_fail, interrupts.c) instead of returning to
# wherever the overrun pointed. -mstack-protector-guard=global: the secret is
# one global word, __stack_chk_guard (entry.asm picks it at boot), not the
# C library's thread-local one — the kernel has no such thing (on x86_64,
# FS and GS hold the program's values).
KERNEL_SSP := -fstack-protector-strong -mstack-protector-guard=global
CFLAGS  := -std=c11 -ffreestanding -O2 -Wall -Wextra -g -march=i686 \
           -fno-tree-loop-distribute-patterns -mgeneral-regs-only $(KERNEL_SSP) \
           -DARCH_I386 -Iinclude -Iarch/i386/include -Ithird_party/uacpi/include $(MODE_FLAGS)
# --no-warn-rwx-segments: the kernel is one read/write/execute blob for now
# (code isn't write-protected; see docs/gaps.md).
LDFLAGS := -T arch/i386/linker.ld -nostdlib -ffreestanding -O2 -Wl,--no-warn-rwx-segments
LIBS    := -lgcc    # gcc's helper routines (e.g. 64-bit division on i386)

# Test and benchmark code goes only into its own build mode.
TEST_SRCS  := kernel/selftest.c kernel/disk_demo.c $(wildcard kernel/*_test.c) \
              drivers/net/wireless/sim.c drivers/net/wireless/sim_crypto.c
BENCH_SRCS := kernel/bench.c
C_SRCS  := $(filter-out $(TEST_SRCS) $(BENCH_SRCS), \
             $(wildcard arch/i386/*.c kernel/*.c drivers/*.c fs/*.c mm/*.c net/*.c \
                        drivers/gpu/drm/*.c drivers/gpu/drm/tiny/*.c drivers/video/*.c \
                        drivers/usb/*.c drivers/hid/*.c drivers/sound/*.c net/wireless/*.c \
                        third_party/uacpi/source/*.c))
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
GEN_OBJS := $(BUILD)/gen/kconfig_default.o
# User test programs (user/*.asm and user/*.c -> ELF executables). Test and
# bench builds put them in the initramfs as /bin/NAME.
# user/musl/hello.c is the ARM64 bring-up's program (its own rule below); left
# in, it would replace user/hello.asm as /bin/hello, which the tests run.
USER_BINS := $(patsubst user/%.asm,$(BUILD)/user/%.elf,$(wildcard user/*.asm)) \
             $(patsubst user/%.c,$(BUILD)/user/%.elf,$(wildcard user/*.c)) \
             $(patsubst user/musl/%.c,$(BUILD)/user/musl/%.elf,\
                 $(filter-out user/musl/hello.c,$(wildcard user/musl/*.c)))
USER_LIB  := $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/mini.o
# user/sys/systest.c, raw system calls for either CPU (rule below): test
# builds carry it too, as /bin/rawsystest.
SYSTEST   := $(BUILD)/user/sys/systest
SNDTEST   := $(BUILD)/user/sys/sndtest
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
             -m64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
             -fno-tree-loop-distribute-patterns $(KERNEL_SSP) -DARCH_X86_64 \
             -Iinclude -Iarch/x86_64/include -Ithird_party/uacpi/include \
             $(MODE_FLAGS) $(NATIVE_CFLAGS)
  LDFLAGS := -T arch/x86_64/linker.ld -nostdlib -ffreestanding -O2 \
             -Wl,--no-warn-rwx-segments $(NATIVE_LDFLAGS)
  LINKER_SCRIPT := arch/x86_64/linker.ld
  NASM_FMT := elf64
  ifeq ($(X86_64_KERNEL),full)
    C_SRCS  := $(filter-out $(TEST_SRCS) $(BENCH_SRCS) arch/x86_64/early.c kernel/elf.c, \
               $(wildcard arch/x86_64/*.c kernel/*.c drivers/*.c fs/*.c mm/*.c net/*.c \
                          drivers/gpu/drm/*.c drivers/gpu/drm/tiny/*.c drivers/video/*.c \
                          drivers/usb/*.c drivers/hid/*.c drivers/sound/*.c net/wireless/*.c \
                          third_party/uacpi/source/*.c))
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

# arm64: hello kernel only (early.c + entry.S). QEMU virt first; Apple Silicon later.
QEMU_ARM64 := qemu-system-aarch64
ifeq ($(ARCH),arm64)
  CFLAGS  := -std=c11 -ffreestanding -O2 -Wall -Wextra -g \
             -mgeneral-regs-only -fno-tree-loop-distribute-patterns \
             -fno-pic -fno-pie -DARCH_AARCH64 \
             -Iarch/aarch64/include -Iinclude $(MODE_FLAGS)
  LDFLAGS := -T arch/aarch64/linker.ld -nostdlib -static -ffreestanding -O2
  LINKER_SCRIPT := arch/aarch64/linker.ld
  C_SRCS  := arch/aarch64/boot.c arch/aarch64/uart.c arch/aarch64/fdt.c \
             arch/aarch64/exceptions.c arch/aarch64/pmm.c arch/aarch64/paging.c \
             arch/aarch64/gic.c arch/aarch64/timer.c arch/aarch64/kheap.c \
             arch/aarch64/kprintf.c arch/aarch64/memcpy.c arch/aarch64/syscall.c \
             arch/aarch64/uaccess.c arch/aarch64/elf64_load.c arch/aarch64/cpio.c \
             arch/aarch64/proc.c arch/aarch64/apple/machine.c \
             arch/aarch64/apple/uart_early.c arch/aarch64/apple/s5l_uart.c \
             arch/aarch64/apple/boot_report.c \
             kernel/string.c
  C_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
  ALPINE_MUSL_ARM64 := build/ext/alpine-arm64-musl
  ALPINE_MUSL_STAMP := $(ALPINE_MUSL_ARM64)/.fetched-musl
  ALPINE_BB_ARM64   := build/ext/alpine-arm64-busybox
  ALPINE_BB_STAMP   := $(ALPINE_BB_ARM64)/.fetched-busybox
  ALPINE_BB_BIN     := $(ALPINE_BB_ARM64)/bin/busybox.static
  ALPINE_ROOT_ARM64 := build/ext/alpine-arm64-root
  ALPINE_ROOT_STAMP := $(ALPINE_ROOT_ARM64)/.fetched-root
  MUSL_ARM64_CFLAGS := -std=c11 -O2 -Wall -Wextra -nostdinc \
                       -isystem $(ALPINE_MUSL_ARM64)/usr/include
  MUSL_ARM64_LIB    := $(ALPINE_MUSL_ARM64)/usr/lib
  OBJS    := $(BUILD)/arch/aarch64/entry.o $(BUILD)/arch/aarch64/vectors.o \
             $(BUILD)/arch/aarch64/mmu.o $(BUILD)/arch/aarch64/user_stub.o \
             $(BUILD)/arch/aarch64/user_write.o $(BUILD)/arch/aarch64/initrd.o \
             $(BUILD)/arch/aarch64/dtb.o $(C_OBJS)
  LIBS    := -lgcc
  VIRT_DTB      := $(BUILD)/virt.dtb
  VIRT_DTB_EMBED := $(BUILD)/virt-embed.dtb
endif

# Intel's Wi-Fi driver, iwlwifi (the MSI's AX201; docs/WIFI-PLAN.md W6):
# Linux's own files, compiled against our Linux API headers as Linux
# compiles them (GNU C, its config switches, compiler_types.h first), and
# linked into the kernel. x86_64 only: no old PC has one. IWLWIFI=0 leaves
# it out (530 KB of code).
IWL_DIR := drivers/net/wireless/intel/iwlwifi
ifeq ($(ARCH),x86_64)
  ifeq ($(X86_64_KERNEL),full)
    IWLWIFI ?= 1
  endif
endif
ifeq ($(IWLWIFI),1)
  IWL_SRCS := $(shell find $(IWL_DIR) -name '*.c')
  IWL_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(IWL_SRCS))
  OBJS     += $(IWL_OBJS)
  C_OBJS   += $(IWL_OBJS)
  CFLAGS   += -DMYOS_IWLWIFI
  $(IWL_OBJS): CFLAGS += -std=gnu11 -D__KERNEL__ -DKBUILD_MODNAME='"iwlwifi"' -I$(IWL_DIR) \
                         -include linux/compiler_types.h -funsigned-char -Wno-override-init \
                         $(addprefix -D,$(addsuffix =1,CONFIG_IWLWIFI CONFIG_IWLMVM CONFIG_PCI \
                           CONFIG_MAC80211 CONFIG_CFG80211 CONFIG_PM)) \
                         -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
                         -Wno-address-of-packed-member -Wno-pointer-sign -Wno-unused-function \
                         -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits
  ifeq ($(IWLDEBUG),1)
    $(IWL_OBJS): CFLAGS += -DCONFIG_IWLWIFI_DEBUG=1
  endif
endif

ifeq ($(WIFISIM),1)
  ifneq ($(MODE),test)                  # (test builds have it already)
    SIM_OBJS := $(BUILD)/drivers/net/wireless/sim.o $(BUILD)/drivers/net/wireless/sim_crypto.o
    OBJS     += $(SIM_OBJS)
    C_OBJS   += $(SIM_OBJS)
  endif
endif
ifeq ($(SLEEPTEST),1)
  ifneq ($(MODE),test)
    OBJS   += $(BUILD)/kernel/fakelid_test.o
    C_OBJS += $(BUILD)/kernel/fakelid_test.o
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
             -netdev user,id=net0$(HOSTFWD) -device e1000,netdev=net0
# make test's forward: the kernel's TCP self-test connects to the host's
# port HAIRPIN_PORT and comes back in to its own listener on port 7
# (kernel/net_test.c). Only that target sets it, so two other QEMUs never
# fight over the port.
HAIRPIN_PORT := 47007

.PHONY: run-uefi test-uefi-shell diskattacks verify-disktests verify-tests attacks all dist utm utm-hd utm-install hd live-root live-extra live-theme live-hd utm-live utm-arm64 run-live run-hd test-hd run run-full run-text debug test check-selftests test-shell bench run-bench usb clean fuzz fuzz-build fuzz-replay diskfuzz diskfuzz-replay verify-racetests uefi run-uefi-text test-uefi live-uefi test-live-uefi test-usb test-usb-hid test-usb-hub test-hid test-battery test-sleep check-sleep test-usb-tablet test-install live-login-initrd test-uefi-login test-arm64 test-arm64-fdt test-arm64-cpio test-arm64-apple test-arm64-apple-boot test-arm64-m1n1-payload m1n1-payload

ifeq ($(ARCH),arm64)
all: $(BUILD)/kernel.elf $(BUILD)/virt.dtb
else ifeq ($(ARCH),x86_64)
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

# uACPI (the AML interpreter, drivers/acpi_os.c) as it comes: its log
# formats say %u for uint32_t, which i686-elf-gcc makes a long.
$(BUILD)/third_party/%.o: CFLAGS += -Wno-format

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f $(NASM_FMT) $< -o $@

-include $(C_OBJS:.o=.d)

# Every kernel object depends on the compiler flags too: change them (a new
# flag such as KERNEL_SSP) and all of the kernel is rebuilt, not only the
# files edited since. cflags.txt is rewritten only when the flags differ.
$(BUILD)/cflags.txt: FORCE
	@mkdir -p $(dir $@)
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
$(C_OBJS) $(IWL_OBJS) $(GEN_OBJS): $(BUILD)/cflags.txt
.PHONY: FORCE
FORCE:

# --- the built-in kernel config ------------------------------------------------
# config/slots.conf becomes the kernel's default config: the same boot block
# and task list a myos-config partition holds (docs/KERNEL-CONFIG.md §10 step
# 2), as two C arrays. kconfig.py stops the build on a mistake in the file,
# and on include/kconfig.h's structs drifting from what it writes. Written
# to .tmp first, so a failure leaves no half-written file.
KCFG_ARCH := $(if $(filter x86_64,$(ARCH)),x86_64,i386)
$(BUILD)/gen/kconfig_default.c: config/slots.conf tools/kconfig/kconfig.py \
                                tools/kconfig/powerfeatures.py include/kconfig.h
	@mkdir -p $(dir $@)
	@python3 tools/kconfig/kconfig.py header > $@.h.tmp
	@awk '/generated by tools\/kconfig\/kconfig.py header/{on=1; next} /end of generated/{on=0} on' \
	  include/kconfig.h | cmp -s - $@.h.tmp \
	  || { echo "include/kconfig.h: its structs differ from 'kconfig.py header'"; rm -f $@.h.tmp; exit 1; }
	@rm -f $@.h.tmp
	python3 tools/kconfig/kconfig.py cdefault $< --arch $(KCFG_ARCH) -o $@.tmp && mv $@.tmp $@

$(BUILD)/gen/kconfig_default.o: $(BUILD)/gen/kconfig_default.c
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(ARCH),arm64)
$(ALPINE_MUSL_STAMP):
	chmod +x tools/fetch-alpine-arm64-musl.sh
	tools/fetch-alpine-arm64-musl.sh $(ALPINE_MUSL_ARM64)

$(BUILD)/user/musl/hello.elf: user/musl/hello.c user/arm64/musl-elf.ld | $(ALPINE_MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_ARM64_CFLAGS) -c user/musl/hello.c -o $(BUILD)/user/musl/hello.o
	$(CC) -nostdlib -static -fno-pic -fno-pie -Wl,-z,noexecstack \
	  -T user/arm64/musl-elf.ld \
	  $(MUSL_ARM64_LIB)/crt1.o $(MUSL_ARM64_LIB)/crti.o $(BUILD)/user/musl/hello.o \
	  -Wl,--start-group $(MUSL_ARM64_LIB)/libc.a -lgcc -Wl,--end-group \
	  $(MUSL_ARM64_LIB)/crtn.o -o $@

$(BUILD)/arch/aarch64/musl_hello.o: arch/aarch64/musl_hello.S $(BUILD)/user/musl/hello.elf
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMUSL_HELLO_ELF=\"$(abspath $(BUILD)/user/musl/hello.elf)\" \
	  -c $< -o $@

$(ALPINE_BB_STAMP):
	chmod +x tools/fetch-alpine-arm64-busybox.sh
	tools/fetch-alpine-arm64-busybox.sh $(ALPINE_BB_ARM64)

$(ALPINE_ROOT_STAMP):
	chmod +x tools/fetch-alpine-arm64-root.sh
	tools/fetch-alpine-arm64-root.sh $(ALPINE_ROOT_ARM64)

$(BUILD)/arm64-initrd.cpio: $(ALPINE_BB_STAMP) $(ALPINE_ROOT_STAMP) tools/pack-arm64-initrd.py
	@mkdir -p $(dir $@)
	python3 tools/pack-arm64-initrd.py $(ALPINE_BB_BIN) $(ALPINE_ROOT_ARM64) $@

$(BUILD)/arch/aarch64/initrd.o: arch/aarch64/initrd.S $(BUILD)/arm64-initrd.cpio
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DARM64_INITRD_PATH=\"$(abspath $(BUILD)/arm64-initrd.cpio)\" \
	  -c $< -o $@
endif

$(BUILD)/kernel.elf: $(OBJS) $(LINKER_SCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

# Strip the ELF wrapper: stage 2 just copies raw bytes into memory.
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# --- the kernel's version ------------------------------------------------------
# YYYYMMDD-g<commit> (the commit's date), "-dirty" when tracked files differ
# from it. uname -v and /proc/version say it; build/kernel-version records it
# with kernel.bin's size and SHA-256, for the live disk's release file (the
# upgrade compares the stick's with the installed disk's: docs/INSTALLER-PLAN.md).
# version.txt is rewritten only when the version changes, so only the two
# files that print it are rebuilt.
MYOS_VERSION := $(shell git log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null)-g$(shell \
                  git rev-parse --short HEAD 2>/dev/null)$(shell \
                  git diff-index --quiet HEAD -- 2>/dev/null || echo -dirty)
$(BUILD)/gen/version.txt: FORCE
	@mkdir -p $(dir $@)
	@echo '$(MYOS_VERSION)' | cmp -s - $@ || echo '$(MYOS_VERSION)' > $@
$(BUILD)/kernel/sys_info.o $(BUILD)/fs/procfs.o $(BUILD)/arch/aarch64/syscall.o: \
	$(BUILD)/gen/version.txt
$(BUILD)/kernel/sys_info.o $(BUILD)/fs/procfs.o $(BUILD)/arch/aarch64/syscall.o: \
	CFLAGS += -DMYOS_VERSION='"$(MYOS_VERSION)"'

$(BUILD)/kernel-version: $(BUILD)/kernel.bin $(BUILD)/gen/version.txt tools/kernel-version.py
	python3 tools/kernel-version.py --version $(MYOS_VERSION) --arch $(ARCH) $< > $@.tmp && mv $@.tmp $@

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
           $(if $(MODE_FLAGS),$(USER_BINS) $(TEST_FILES) $(SYSTEST)) Makefile
	@rm -rf $(BUILD)/initramfs
	@mkdir -p $(BUILD)/initramfs/bin $(BUILD)/initramfs/usr/lib $(BUILD)/initramfs/root \
	          $(BUILD)/initramfs/tmp $(BUILD)/initramfs/lib $(BUILD)/initramfs/etc/terminfo
	cp -R initramfs/. $(BUILD)/initramfs/
ifneq ($(MODE_FLAGS),)
	@mkdir -p $(BUILD)/initramfs/test
	for f in $(USER_BINS); do cp $$f $(BUILD)/initramfs/bin/$$(basename $$f .elf); done
	cp $(SYSTEST) $(BUILD)/initramfs/bin/rawsystest
	cp $(ALPINE)/usr/sbin/iw $(BUILD)/initramfs/bin/iw
	cp -P $(ALPINE)/usr/lib/libnl-3.so.* $(ALPINE)/usr/lib/libnl-genl-3.so.* $(BUILD)/initramfs/usr/lib/
	cp $(ALPINE)/sbin/wpa_supplicant $(ALPINE)/sbin/wpa_cli $(BUILD)/initramfs/bin/
	cp $(ALPINE)/lib/libcrypto.so.3 $(ALPINE)/lib/libssl.so.3 $(BUILD)/initramfs/lib/
	cp -P $(ALPINE)/usr/lib/libdbus-1.so.* $(ALPINE)/usr/lib/libpcsclite.so.1 \
	      $(ALPINE)/usr/lib/libpcsclite_real.so.1 $(BUILD)/initramfs/usr/lib/
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

# The 8x16 font the UEFI loader gives the kernel console (EFI/MYOS/FONT.BIN):
# taken from QEMU's VGA BIOS on this machine (tools/mkfont.py), not kept here.
arch/i386/boot/efi/vga-font.bin: tools/mkfont.py
	python3 tools/mkfont.py $@

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
	END=0x$$($(NM) $(BUILD)/kernel.elf | awk '$$3 == "__kernel_end_phys" { print $$1 }'); \
	LOAD=$$(( (END + 4095) / 4096 * 4096 )); \
	$(CLANG_EFI) $(if $(filter 1,$(TRACE)),-DLOADER_BARS) -DKERNEL_LOAD=0x100000 -DKERNEL_END=$$END -DINITRD_LOAD=$$LOAD \
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
	$(CLANG_EFI) $(if $(filter 1,$(TRACE)),-DLOADER_BARS) -DINITRD_LOAD=$$LOAD -I$(BUILD)/efi -Iarch/i386/boot/efi \
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
# 4 GiB, as on a real laptop: the firmware then puts its stack and our initrd
# above 2 GiB, which is how two boot bugs hid at 256 MiB (docs/LOG.md, 2026-10-05).
QEMU_UEFI_DISK = $(UEFI_IMG)
# QEMU_UEFI_MEM=8G puts 6 GiB above the low zone (the high zone, pmm.h).
QEMU_UEFI_MEM ?= 4G
# QEMU_UEFI_CPU=max: a CPU with SMEP, SMAP and NX, as the new PCs have.
QEMU_UEFI_CPU ?= qemu64
QEMU_UEFI_FLAGS = -machine q35 -m $(QEMU_UEFI_MEM) -cpu $(QEMU_UEFI_CPU) \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	-drive if=none,format=raw,file=$(QEMU_UEFI_DISK),id=bootdisk \
	-device ide-hd,drive=bootdisk,bootindex=0 \
	-net none -no-reboot

# In a window: type at the shell there; kernel messages also go to this terminal.
run-uefi: $(UEFI_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	$(QEMU64) $(QEMU_UEFI_FLAGS) -serial stdio

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

# QEMU "virt" ARM64 hello (roadmap step 14; Apple Silicon bare metal is step 15).
# Do not mix -nographic with -serial file: — both claim uart0; the log stays empty.
# DTB is embedded in the kernel (arch/aarch64/dtb.S); ELF -kernel has no DTB in RAM.
QEMU_ARM64_BASE = -machine virt -cpu max -m 256M

$(BUILD)/virt.dtb:
	@mkdir -p $(BUILD)
	$(QEMU_ARM64) -machine virt,dumpdtb=$@ -display none 2>/dev/null

# dumpdtb sets totalsize to 1 MiB; embed 128 KiB and patch the header to match.
$(BUILD)/virt-embed.dtb: $(BUILD)/virt.dtb
	@dd if=$< of=$@ bs=1 count=131072 2>/dev/null
	@python3 -c 'import sys,struct; d=bytearray(open(sys.argv[1],"rb").read()); \
	  d[4:8]=struct.pack(">I",131072); open(sys.argv[1],"wb").write(d)' $@

$(BUILD)/arch/aarch64/dtb.o: arch/aarch64/dtb.S $(BUILD)/virt-embed.dtb
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMYOS_DTB_PATH=\"$(abspath $(VIRT_DTB_EMBED))\" \
	  -c $< -o $@

$(BUILD)/arch/aarch64/boot-harness.o: arch/aarch64/boot.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DAPPLE_HARNESS_FALLBACK -MMD -MP -c $< -o $@

$(BUILD)/arch/aarch64/harness_dtb.o: arch/aarch64/apple/harness_dtb.S $(BUILD)/m1-qemu-harness.dtb
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DARM64_HARNESS_DTB_PATH=\"$(abspath $(BUILD)/m1-qemu-harness.dtb)\" \
	  -c $< -o $@

ARM64_HARNESS_OBJS := $(filter-out $(BUILD)/arch/aarch64/boot.o,$(OBJS)) \
	$(BUILD)/arch/aarch64/boot-harness.o $(BUILD)/arch/aarch64/harness_dtb.o

$(BUILD)/kernel-apple-harness.elf: $(ARM64_HARNESS_OBJS) $(LINKER_SCRIPT)
	$(CC) $(LDFLAGS) $(ARM64_HARNESS_OBJS) -o $@ $(LIBS)

$(BUILD)/arch/aarch64/entry-m1n1-sim.o: arch/aarch64/entry.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DM1N1_SIMULATE_X0 -c $< -o $@

ARM64_M1N1_SIM_OBJS := $(filter-out $(BUILD)/arch/aarch64/entry.o,$(OBJS)) \
	$(BUILD)/arch/aarch64/entry-m1n1-sim.o $(BUILD)/arch/aarch64/harness_dtb.o

$(BUILD)/kernel-m1n1-sim.elf: $(ARM64_M1N1_SIM_OBJS) $(BUILD)/m1-qemu-harness.dtb $(LINKER_SCRIPT)
	$(CC) $(LDFLAGS) $(ARM64_M1N1_SIM_OBJS) -o $@ $(LIBS)

run-arm64: $(BUILD)/kernel.elf
	chmod +x tools/run-arm64.sh
	tools/run-arm64.sh

# Host-side unit test (FDT /memory on virt-embed.dtb).
test-arm64-fdt: $(VIRT_DTB_EMBED)
	@mkdir -p $(BUILD)
	gcc -Wall -Wextra -Iarch/aarch64/include -o $(BUILD)/test-arm64-fdt \
	  tools/test-arm64-fdt.c arch/aarch64/fdt.c
	$(BUILD)/test-arm64-fdt $(abspath $(VIRT_DTB_EMBED))

test-arm64-cpio:
	@mkdir -p $(BUILD)
	gcc -Wall -Wextra -Iarch/aarch64/include -o $(BUILD)/test-arm64-cpio \
	  tools/test-arm64-cpio.c arch/aarch64/cpio.c
	$(BUILD)/test-arm64-cpio

$(BUILD)/m1-min.dtb: arch/aarch64/apple/fixtures/m1-min.dts
	@mkdir -p $(BUILD)
	dtc -I dts -O dtb -o $@ $<

$(BUILD)/m1-qemu-harness.dtb: arch/aarch64/apple/fixtures/m1-qemu-harness.dts
	@mkdir -p $(BUILD)
	dtc -I dts -O dtb -o $@ $<

test-arm64-apple: $(VIRT_DTB_EMBED) $(BUILD)/m1-min.dtb $(BUILD)/m1-qemu-harness.dtb
	@mkdir -p $(BUILD)
	gcc -Wall -Wextra -Iarch/aarch64/include -o $(BUILD)/test-arm64-apple \
	  tools/test-arm64-apple.c arch/aarch64/fdt.c arch/aarch64/apple/machine.c
	$(BUILD)/test-arm64-apple $(abspath $(VIRT_DTB_EMBED)) \
	  $(abspath $(BUILD)/m1-min.dtb) $(abspath $(BUILD)/m1-qemu-harness.dtb)

test-arm64-mmio-map: $(BUILD)/m1-min.dtb
	@mkdir -p $(BUILD)
	gcc -Wall -Wextra -Iarch/aarch64/include -o $(BUILD)/test-arm64-mmio-map \
	  tools/test-arm64-mmio-map.c arch/aarch64/apple/uart_early.c \
	  arch/aarch64/fdt.c arch/aarch64/apple/machine.c
	$(BUILD)/test-arm64-mmio-map $(abspath $(BUILD)/m1-min.dtb)

test-arm64-uart-early: $(BUILD)/m1-min.dtb $(BUILD)/m1-qemu-harness.dtb
	@mkdir -p $(BUILD)
	gcc -Wall -Wextra -Iarch/aarch64/include -o $(BUILD)/test-arm64-uart-early \
	  tools/test-arm64-uart-early.c arch/aarch64/apple/uart_early.c \
	  arch/aarch64/fdt.c arch/aarch64/apple/machine.c
	$(BUILD)/test-arm64-uart-early $(abspath $(BUILD)/m1-min.dtb) \
	  $(abspath $(BUILD)/m1-qemu-harness.dtb)

m1n1-payload: $(BUILD)/kernel.elf
	chmod +x tools/mk-m1n1-payload.sh
	tools/mk-m1n1-payload.sh $(BUILD)/kernel.elf $(BUILD)/m1n1

test-arm64-m1n1-payload: m1n1-payload
	chmod +x tools/test-arm64-m1n1-payload.sh
	tools/test-arm64-m1n1-payload.sh

test-arm64-apple-boot: $(BUILD)/kernel-apple-harness.elf $(BUILD)/m1-qemu-harness.dtb
	@rm -f $(BUILD)/arm64-apple-serial.log
	@(sleep 8; echo quit) \
	  | $(QEMU_ARM64) $(QEMU_ARM64_BASE) -display none \
	    -kernel $(BUILD)/kernel-apple-harness.elf \
	    -serial file:$(BUILD)/arm64-apple-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/arm64-apple-serial.log; echo
	@grep -q "MyOS aarch64 (Apple MacBook" $(BUILD)/arm64-apple-serial.log \
	  || { echo "FAIL: Apple DTB platform banner missing"; exit 1; }
	@grep -q "Boot: x0=" $(BUILD)/arm64-apple-serial.log \
	  || { echo "FAIL: Boot report line missing"; exit 1; }
	@grep -q "embedded" $(BUILD)/arm64-apple-serial.log \
	  || { echo "FAIL: harness should use embedded DTB in QEMU"; exit 1; }
	@grep -q "RAM: 0x40000000" $(BUILD)/arm64-apple-serial.log \
	  || { echo "FAIL: RAM line with Apple harness DTB"; exit 1; }
	@grep -q "bash-5.2" $(BUILD)/arm64-apple-serial.log \
	  || { echo "FAIL: bash prompt missing on Apple harness DTB"; exit 1; }
	@echo "ARM64 Apple harness OK: DTB hand-off + apple,uart + bash"

test-arm64-m1n1-x0-sim: $(BUILD)/kernel-m1n1-sim.elf
	@rm -f $(BUILD)/arm64-m1n1-x0-serial.log
	@(sleep 8; echo quit) \
	  | $(QEMU_ARM64) $(QEMU_ARM64_BASE) -display none \
	    -kernel $(BUILD)/kernel-m1n1-sim.elf \
	    -serial file:$(BUILD)/arm64-m1n1-x0-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/arm64-m1n1-x0-serial.log; echo
	@grep -q "MyOS aarch64 (Apple MacBook" $(BUILD)/arm64-m1n1-x0-serial.log \
	  || { echo "FAIL: Apple platform from x0 DTB"; exit 1; }
	@grep -q "Boot: x0=" $(BUILD)/arm64-m1n1-x0-serial.log \
	  || { echo "FAIL: Boot report line missing"; exit 1; }
	@grep -q "(x0)" $(BUILD)/arm64-m1n1-x0-serial.log \
	  || { echo "FAIL: expected DTB via x0 (m1n1 hand-off), not embedded fallback"; exit 1; }
	@grep -q "bash-5.2" $(BUILD)/arm64-m1n1-x0-serial.log \
	  || { echo "FAIL: bash prompt missing on m1n1 x0 sim"; exit 1; }
	@echo "ARM64 m1n1 x0 sim OK: entry x0 DTB + Apple platform + bash"

test-arm64: test-arm64-fdt test-arm64-cpio test-arm64-apple test-arm64-mmio-map \
	test-arm64-uart-early \
	test-arm64-m1n1-payload test-arm64-apple-boot test-arm64-m1n1-x0-sim $(BUILD)/kernel.elf
	@rm -f $(BUILD)/arm64-serial.log
	@(sleep 8; echo quit) \
	  | $(QEMU_ARM64) $(QEMU_ARM64_BASE) -display none \
	    -kernel $(BUILD)/kernel.elf \
	    -serial file:$(BUILD)/arm64-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/arm64-serial.log; echo
	@grep -q "MyOS aarch64 (QEMU virt)" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: ARM64 platform line missing"; exit 1; }
	@grep -q "Boot: x0=" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: Boot report line missing"; exit 1; }
	@grep -q "Hello world from MyOS" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: ARM64 hello did not run on virt"; exit 1; }
	@grep -q "FDT: magic ok" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: device tree not parsed"; exit 1; }
	@grep -q "RAM: 0x40000000" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: RAM line missing"; exit 1; }
	@grep -q "PMM:" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: PMM line missing"; exit 1; }
	@grep -q "MMU: on" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: MMU line missing"; exit 1; }
	@grep -q "Timer:" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: timer line missing"; exit 1; }
	@grep -q "kheap: kmalloc" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: kheap line missing"; exit 1; }
	@grep -q "EL0: data access ok" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: EL0 data access"; exit 1; }
	@grep -q "initramfs: /init" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: initramfs /init missing"; exit 1; }
	@grep -q "cat from initramfs" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: BusyBox cat /hello.txt missing"; exit 1; }
	@grep -q "bash from MyOS" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: bash login banner missing"; exit 1; }
	@grep -q "bash-5.2" $(BUILD)/arm64-serial.log \
	  || { echo "FAIL: bash prompt missing"; exit 1; }
	@echo "ARM64 OK: FDT + PMM + MMU + timer + kheap + initramfs /init + bash on QEMU virt"

utm-arm64: $(BUILD)/kernel.elf
	chmod +x tools/mkutm-arm64.sh
	@mkdir -p $(BUILD)
	tools/mkutm-arm64.sh $(BUILD)/kernel.elf $(BUILD)/MyOS-ARM64.utm
	@echo "Open $(BUILD)/MyOS-ARM64.utm in UTM (serial terminal)."

# user/sys/systest.c: the system call layer checked from a program's side,
# with raw syscalls (no C library), for whichever CPU ARCH is. It goes only
# into a test copy of the initramfs and UEFI image (and `make test`'s), never
# the normal ones. (SYSTEST is named near USER_BINS.)
TEST_INITRD  := $(BUILD)/initrd-test.cpio
UEFI_TEST_IMG := $(BUILD)/myos-uefi-test.img
ifeq ($(ARCH),x86_64)
  SYSTEST_FLAGS := -m64
else
  SYSTEST_FLAGS := -march=i686
endif

$(SYSTEST): user/sys/systest.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -ffreestanding -nostdlib -static -O2 -Wall -Wextra -fno-pic -fno-pie \
	  $(SYSTEST_FLAGS) $< -o $@ -lgcc

# user/sys/sndtest.c: the sound card through raw ALSA ioctls (test-sound).
$(SNDTEST): user/sys/sndtest.c include/sound/asound.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -ffreestanding -nostdlib -static -O2 -Wall -Wextra -fno-pic -fno-pie \
	  $(SYSTEST_FLAGS) $< -o $@ -lgcc

$(TEST_INITRD): $(INITRD) $(SYSTEST) $(SNDTEST)
	@rm -rf $(BUILD)/initramfs-test
	@# -p keeps the modes: plain -R applies the umask, and /tmp's 1777
	@# became 1755 (users could not write there).
	cp -Rp $(BUILD)/initramfs $(BUILD)/initramfs-test
	cp $(SYSTEST) $(BUILD)/initramfs-test/bin/systest
	cp $(SNDTEST) $(BUILD)/initramfs-test/bin/sndtest
	cd $(BUILD)/initramfs-test && find . -mindepth 1 ! -name .DS_Store | sort \
	  | cpio -o -H newc -R 0:0 --quiet > ../$(notdir $(TEST_INITRD))

# The kernel config the test disk carries (tools/kconfig/test.conf,
# generation 3): the kernel must find it and use it instead of its own.
$(BUILD)/kconfig-test.img: tools/kconfig/test.conf tools/kconfig/kconfig.py
	python3 tools/kconfig/kconfig.py partition $< --arch $(KCFG_ARCH) --generation 3 -o $@ > /dev/null

$(UEFI_TEST_IMG): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(TEST_INITRD) \
                  arch/i386/boot/efi/vga-font.bin tools/mkuefi.py $(BUILD)/kconfig-test.img
	python3 tools/mkuefi.py --out $@ --esp-mb 64 --config-file $(BUILD)/kconfig-test.img \
	  --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	  --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	  --file EFI/MYOS/INITRD.CPIO=$(TEST_INITRD) \
	  --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin

# The kernel's user space under UEFI: /init and a BusyBox shell, with commands
# typed at the PS/2 keyboard as in test-shell. The x86_64 milestone (64-bit
# programs on the 64-bit kernel); the i386 image passes it too.
UEFI_SHELL_CMDS := "uname -m" "echo hello from user space" "ls /" "echo hi | wc -c" \
                   "mkdir /tmp/x && echo data > /tmp/x/f && cat /tmp/x/f" \
                   "sh -c 'exit 3'; echo status \$$?" \
                   "yes | head -2 | wc -l" \
                   "trap 'echo caught usr1' USR1; kill -USR1 \$$\$$; echo after trap" \
                   "sh -c 'kill -SEGV \$$\$$'; echo killed \$$?" \
                   "awk 'BEGIN { print 1.5 * 3 }'"
# Also an NVMe disk (a snapshot of the FAT32 test disk): on x86_64 it must run
# on MSI-X (the local APIC, docs/WIFI-PLAN.md W0a) and be read through it.
# OVMF puts its registers above 4 GiB, where i386 must say it can't map them.
test-uefi-shell: QEMU_UEFI_DISK = $(UEFI_TEST_IMG)
test-uefi-shell: $(UEFI_TEST_IMG) $(USB)
	@rm -f $(BUILD)/uefi-shell.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 15; tools/qemu-type.sh systest | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 8; for c in $(UEFI_SHELL_CMDS); do \
	    tools/qemu-type.sh "$$c" | while read k; do echo "$$k"; sleep 0.05; done; sleep 2; \
	  done; \
	  tools/qemu-type.sh "sleep 30" | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  echo "sendkey ctrl-c"; sleep 1; \
	  tools/qemu-type.sh "echo after ctrl-c" | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 2; echo quit) \
	  | $(QEMU64) $(QEMU_UEFI_FLAGS) -display none \
	    -drive if=none,format=raw,file=$(USB),id=nvme0,snapshot=on \
	    -device nvme,drive=nvme0,serial=myos-nvme \
	    -serial file:$(BUILD)/uefi-shell.log -monitor stdio >/dev/null || true
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/uefi-shell.log > $(BUILD)/uefi-shell.txt
	@sed -n '/Welcome/,$$p' $(BUILD)/uefi-shell.txt; echo
	@L=$(BUILD)/uefi-shell.txt; ok=1; \
	  grep -q "^Welcome\|Welcome to MyOS" $$L || { echo "FAIL: /init did not run"; ok=0; }; \
	  grep -q "^systest: all ok" $$L || { echo "FAIL: systest (the syscall checks)"; ok=0; }; \
	  grep -q "kconfig: [a-z0-9]*3 copy A, generation 3: soft realtime: audio 2 12; brakes 4 9 14 19" $$L \
	    || { echo "FAIL: the kernel config on the test disk (myos-config) was not used"; ok=0; }; \
	  grep -q "^$(UNAME_M)[[:space:]]*$$" $$L || { echo "FAIL: uname -m"; ok=0; }; \
	  grep -q "^hello from user space" $$L || { echo "FAIL: echo"; ok=0; }; \
	  grep -q "README.txt" $$L || { echo "FAIL: ls"; ok=0; }; \
	  grep -q "^3[[:space:]]*$$" $$L || { echo "FAIL: echo | wc -c"; ok=0; }; \
	  grep -q "^data[[:space:]]*$$" $$L || { echo "FAIL: write + read a file"; ok=0; }; \
	  grep -q "^status 3" $$L || { echo "FAIL: exit status"; ok=0; }; \
	  grep -q "^2[[:space:]]*$$" $$L || { echo "FAIL: SIGPIPE (yes | head)"; ok=0; }; \
	  ! grep -q "Broken pipe" $$L || { echo "FAIL: SIGPIPE not delivered"; ok=0; }; \
	  grep -q "^caught usr1" $$L || { echo "FAIL: signal handler"; ok=0; }; \
	  grep -q "^after trap" $$L || { echo "FAIL: sigreturn"; ok=0; }; \
	  grep -q "^killed 139" $$L || { echo "FAIL: death by signal"; ok=0; }; \
	  grep -q "^4.5" $$L || { echo "FAIL: floating point"; ok=0; }; \
	  grep -q "^after ctrl-c" $$L || { echo "FAIL: Ctrl-C"; ok=0; }; \
	  if [ "$(ARCH)" = x86_64 ]; then \
	    grep -q "^ *nvme: nvme0n1: .*MSI-X irq [0-9]" $$L || { echo "FAIL: NVMe on MSI-X"; ok=0; }; \
	    grep -q "^ *fat32: nvme0n1.* mounted" $$L || { echo "FAIL: read through MSI-X"; ok=0; }; \
	  else \
	    grep -q "^ *nvme: cannot map .*above 4 GiB" $$L || { echo "FAIL: NVMe above 4 GiB not reported"; ok=0; }; \
	  fi; \
	  [ $$ok = 1 ] && echo "UEFI SHELL TEST OK ($(ARCH))" || exit 1

# Sound (docs/SOUND-PLAN.md): QEMU's Intel HD Audio controller with a
# laptop-like codec (hda-micro: a speaker and a mic) whose speaker is
# written to a WAV file. sndtest plays 440 Hz through write() and through
# the mapped buffer; tools/check-tone.py finds the tone in the file.
SOUND_WAV := $(BUILD)/sound-test.wav
test-sound: QEMU_UEFI_DISK = $(UEFI_TEST_IMG)
test-sound: $(UEFI_TEST_IMG)
	@rm -f $(BUILD)/sound.log $(SOUND_WAV)
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 15; for c in "ls -l /dev/snd" sndtest; do \
	    tools/qemu-type.sh "$$c" | while read k; do echo "$$k"; sleep 0.05; done; sleep 2; \
	  done; sleep 10; echo quit) \
	  | $(QEMU64) $(QEMU_UEFI_FLAGS) -display none \
	    -audiodev wav,id=snd0,path=$(SOUND_WAV) \
	    -device ich9-intel-hda -device hda-micro,audiodev=snd0 \
	    -serial file:$(BUILD)/sound.log -monitor stdio >/dev/null || true
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/sound.log > $(BUILD)/sound.txt
	@grep -E "^ *(hda|sound):|^sndtest|^ok |^FAIL|^c.* (control|pcm)C" $(BUILD)/sound.txt; echo
	@L=$(BUILD)/sound.txt; ok=1; \
	  grep -q "^ *sound: card 0: " $$L || { echo "FAIL: no sound card"; ok=0; }; \
	  grep -q "^sndtest: all ok" $$L || { echo "FAIL: sndtest"; ok=0; }; \
	  python3 tools/check-tone.py $(SOUND_WAV) 440 0.8 || ok=0; \
	  [ $$ok = 1 ] && echo "SOUND TEST OK ($(ARCH))" || exit 1

# An absolute pointer (QEMU's usb-tablet: how UTM hands over the Mac's
# pointer, and what a USB touch screen looks like) reaches /dev/input/event2:
# its place and its button (tools/test-tablet.py).
test-usb-tablet: $(UEFI_IMG)
	@[ "$(ARCH)" = x86_64 ] || { echo "test-usb-tablet: make ARCH=x86_64 test-usb-tablet"; exit 1; }
	python3 tools/test-tablet.py $(UEFI_IMG) $(OVMF_CODE) $(OVMF_VARS) $(BUILD)

# USB keyboard and mouse on a PC with no PS/2 controller at all (as on the
# GPD P2 Max): the shell is typed at with QEMU's usb-kbd (12 Mb/s, as laptop
# keyboards are), and /dev/input/event1 must get usb-mouse's move, right
# click and wheel. An input event is 24 bytes on x86_64, 16 on i386; od
# prints it as 16-bit numbers ending in type, code, value. x86_64 boots by
# UEFI; i386 by BIOS, as the old PCs do (and OVMF puts the xHCI's registers
# above 4 GiB, where the i386 kernel can't reach), with SMM off: SeaBIOS
# polls its USB keyboard through SMM, which hangs once the boot loader has
# switched to the picture's mode (docs/gaps.md, Boot). docs/USB-PLAN.md U5.
ifeq ($(ARCH),x86_64)
EV = 24
HID_BOOT = $(QEMU64) -machine q35,i8042=off -m 1G -cpu qemu64 \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
    -drive if=none,format=raw,file=$(UEFI_IMG),id=bootdisk,snapshot=on \
    -device ide-hd,drive=bootdisk,bootindex=0
test-usb-hub: $(UEFI_IMG)
test-usb-hid: $(UEFI_IMG)
else
EV = 16
HID_BOOT = $(QEMU) -machine pc,i8042=off,smm=off -m 64M -cpu pentium3 \
    -drive format=raw,file=$(IMG),index=0,media=disk,snapshot=on
test-usb-hub: $(IMG)
test-usb-hid: $(IMG)
endif
	@rm -f $(BUILD)/usb-hid.log
	@[ ! -f $(OVMF_VARS) ] || cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(for i in $$(seq 60); do sleep 1; \
	    grep -q "Welcome to MyOS" $(BUILD)/usb-hid.log 2>/dev/null && break; done; sleep 1; \
	  tools/qemu-type.sh 'echo USB-KBD $$((6*7)) ~ok; head -c $$((9*$(EV))) /dev/input/event1 | od -An -v -tu2 -w$(EV) > /tmp/m &' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  echo "mouse_move 10 5"; sleep 0.3; echo "mouse_button 2"; sleep 0.3; \
	  echo "mouse_button 0"; sleep 0.3; echo "mouse_move 0 0 1"; sleep 1; \
	  tools/qemu-type.sh 'echo EVENTS; cat /tmp/m; echo END' | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 2; echo quit) \
	  | $(HID_BOOT) \
	    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,usb_version=1 \
	    -device usb-mouse,bus=xhci.0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/usb-hid.log -monitor stdio >/dev/null || true
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/usb-hid.log > $(BUILD)/usb-hid.txt
	@L=$(BUILD)/usb-hid.txt; E=$(BUILD)/usb-hid-events.txt; ok=1; \
	  sed -n '/^EVENTS/,/^END/p' $$L | tr -s ' ' > $$E; \
	  grep -a "PS/2\|usb\|USB-KBD" $$L; cat $$E; \
	  grep -q "^ *keyboard: no PS/2 controller" $$L || { echo "FAIL: no PS/2 controller seen"; ok=0; }; \
	  grep -q "^ *usb-hid: .* Keyboard: a keyboard" $$L || { echo "FAIL: the USB keyboard found"; ok=0; }; \
	  grep -q "^ *usb-hid: .* Mouse: a mouse" $$L || { echo "FAIL: the USB mouse found"; ok=0; }; \
	  grep -q "^USB-KBD 42 ~ok" $$L || { echo "FAIL: typing on the USB keyboard"; ok=0; }; \
	  grep -q " 2 0 10 0$$" $$E || { echo "FAIL: mouse moved right 10"; ok=0; }; \
	  grep -q " 2 1 5 0$$" $$E || { echo "FAIL: mouse moved down 5"; ok=0; }; \
	  grep -q " 1 273 1 0$$" $$E || { echo "FAIL: right button down"; ok=0; }; \
	  grep -q " 1 273 0 0$$" $$E || { echo "FAIL: right button up"; ok=0; }; \
	  grep -q " 2 8 1 0$$" $$E || { echo "FAIL: wheel up"; ok=0; }; \
	  [ $$ok = 1 ] && echo "USB HID TEST OK ($(ARCH)): USB keyboard and mouse, no PS/2" || exit 1

# The HID parser on the Mac: a precision touchpad (the GPD P2 Max's) moves the
# pointer like a mouse, taps click, palms don't (tools/test-hid.c; QEMU has no
# USB touchpad to try it on).
test-hid:
	@mkdir -p $(BUILD)/host
	cc -std=c11 -Wall -Wextra -Werror -c -Itools/fuzz/shim -Iinclude drivers/hid/hid.c -o $(BUILD)/host/hid.o
	cc -std=c11 -Wall -Wextra -Werror tools/test-hid.c $(BUILD)/host/hid.o -o $(BUILD)/host/test-hid
	$(BUILD)/host/test-hid

# The battery (docs/BATTERY-PLAN.md): QEMU has none, so tools/fakebat.py
# makes an ACPI table with one (50 Wh new, 45 Wh now, discharging at 9 W,
# running down 0.1 Wh each time it is read) and a charger, unplugged. The
# AML interpreter must find both, /sys/class/power_supply must show them
# as Linux does, the numbers must move with the 10-second reading, and
# /sys/firmware/acpi/platform_profile must switch (and be given to wheel,
# as the live system's boot does). i386 by BIOS, x86_64 by UEFI.
ifeq ($(ARCH),x86_64)
BAT_BOOT = $(QEMU64) -machine q35 -m 1G -cpu qemu64 \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
    -drive if=none,format=raw,file=$(UEFI_IMG),id=bootdisk,snapshot=on \
    -device ide-hd,drive=bootdisk,bootindex=0
test-battery: $(UEFI_IMG)
else
BAT_BOOT = $(QEMU) -m 64M -cpu pentium3 -drive format=raw,file=$(IMG),index=0,media=disk,snapshot=on
test-battery: $(IMG)
endif
	@rm -f $(BUILD)/battery.log
	@[ ! -f $(OVMF_VARS) ] || cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	python3 tools/fakebat.py $(BUILD)/fakebat.aml
	@(for i in $$(seq 60); do sleep 1; \
	    grep -q "Welcome to MyOS" $(BUILD)/battery.log 2>/dev/null && break; done; sleep 1; \
	  tools/qemu-type.sh 'mkdir -p /sys; mount -t sysfs none /sys; cd /sys/class/power_supply; grep . */*' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  tools/qemu-type.sh 'a=$$(cat BAT0/energy_now); sleep 11; b=$$(cat BAT0/energy_now); [ $$b -lt $$a ] && echo DRAIN"S"' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 20; \
	  tools/qemu-type.sh 'cd /sys/firmware/acpi; cat platform_profile_choices; echo low-power > platform_profile; echo NOW $$(cat platform_profile); echo performance > platform_profile; echo balanced > platform_profile; echo THEN $$(cat platform_profile)' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  tools/qemu-type.sh 'chgrp 10 platform_profile; chmod 664 platform_profile; ls -ln platform_profile' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 1; \
	  tools/qemu-type.sh 'cd /sys/class/backlight/acpi_video0; grep . *; echo 3 > brightness; sleep 1; echo BL $$(cat actual_brightness) $$(cat brightness)' \
	    | while read k; do echo "$$k"; sleep 0.05; done; sleep 2; echo quit) \
	  | $(BAT_BOOT) -acpitable file=$(BUILD)/fakebat.aml \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/battery.log -monitor stdio >/dev/null || true
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/battery.log > $(BUILD)/battery.txt
	@L=$(BUILD)/battery.txt; ok=1; \
	  grep -a "acpi\|battery\|BAT0\|AC/\|DRAINS\|NOW\|cpupower: profile\|platform_profile\|backlight\|brightness\|^type:\|^BL" $$L; \
	  grep -q "^acpi: the AML is running" $$L || { echo "FAIL: the AML interpreter started"; ok=0; }; \
	  grep -q "^battery BAT0: MyOS FAKE-BAT, 50000 mWh new, 45000 now" $$L || { echo "FAIL: the battery found"; ok=0; }; \
	  grep -q "^AC/online:0" $$L || { echo "FAIL: the charger, unplugged"; ok=0; }; \
	  grep -q "^AC/type:Mains" $$L || { echo "FAIL: the charger's type"; ok=0; }; \
	  grep -q "^BAT0/status:Discharging" $$L || { echo "FAIL: discharging"; ok=0; }; \
	  grep -q "^BAT0/capacity:6[67]" $$L || { echo "FAIL: capacity (30 of 45 Wh)"; ok=0; }; \
	  grep -q "^BAT0/energy_full_design:50000000" $$L || { echo "FAIL: design energy, in uWh"; ok=0; }; \
	  grep -q "^BAT0/energy_full:45000000" $$L || { echo "FAIL: full energy"; ok=0; }; \
	  grep -q "^BAT0/power_now:9000000" $$L || { echo "FAIL: power, in uW"; ok=0; }; \
	  grep -q "^BAT0/cycle_count:127" $$L || { echo "FAIL: cycles"; ok=0; }; \
	  grep -q "^BAT0/technology:Li-ion" $$L || { echo "FAIL: technology"; ok=0; }; \
	  grep -q "^BAT0/model_name:FAKE-BAT" $$L || { echo "FAIL: model"; ok=0; }; \
	  grep -q "DRAINS" $$L || { echo "FAIL: read again every 10 seconds"; ok=0; }; \
	  grep -q "^low-power balanced performance" $$L || { echo "FAIL: the profile choices"; ok=0; }; \
	  grep -q "^cpupower: profile low-power" $$L || { echo "FAIL: the profile applied"; ok=0; }; \
	  grep -q "^NOW low-power" $$L || { echo "FAIL: the profile reads back"; ok=0; }; \
	  grep -q "^THEN balanced" $$L || { echo "FAIL: performance, then balanced"; ok=0; }; \
	  grep -q "^-rw-rw-r-- *1 0 *10 .*platform_profile" $$L || { echo "FAIL: the profile given to wheel"; ok=0; }; \
	  grep -q "^backlight: 11 levels, 5% to 100%, now 100%" $$L || { echo "FAIL: the panel's _BCL, sorted"; ok=0; }; \
	  grep -q "^max_brightness:10" $$L || { echo "FAIL: max_brightness"; ok=0; }; \
	  grep -q "^type:firmware" $$L || { echo "FAIL: the backlight's type"; ok=0; }; \
	  grep -q "^BL 3 3" $$L || { echo "FAIL: brightness 3 set through _BCM"; ok=0; }; \
	  [ $$ok = 1 ] && echo "BATTERY TEST OK ($(ARCH))" || exit 1

# Sleep (docs/SLEEP-PLAN.md): QEMU has no lid, so tools/fakelid.py makes an
# ACPI table with one, and a SLEEPTEST=1 kernel (a build folder of its own)
# opens and shuts it when a CPU is added (device_add). tools/test-sleep.py
# plays the power daemon from the host: the lid shuts -> sleep; a key with
# the lid shut -> still asleep; the lid opens -> awake; then a key and the
# power button wake it. A busy loop must stop while asleep (QEMU's CPU time
# drops), the wall clock must jump and the uptime not, and the lid and the
# power button must reach /dev/input/event3 and event4. i386 by BIOS,
# x86_64 by UEFI.
test-sleep:
	@$(MAKE) --no-print-directory SLEEPTEST=1 check-sleep
ifeq ($(ARCH),x86_64)
SLEEP_BOOT = $(QEMU64) -machine q35 -m 1G -cpu qemu64 \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
    -drive if=none,format=raw,file=$(UEFI_IMG),id=bootdisk,snapshot=on \
    -device ide-hd,drive=bootdisk,bootindex=0
check-sleep: $(UEFI_IMG)
else
SLEEP_BOOT = $(QEMU) -m 64M -cpu pentium3 -drive format=raw,file=$(IMG),index=0,media=disk,snapshot=on
check-sleep: $(IMG)
endif
	@[ ! -f $(OVMF_VARS) ] || cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	python3 tools/fakelid.py $(BUILD)/fakelid.aml
	python3 tools/test-sleep.py $(ARCH) $(BUILD)/sleep.log $(SLEEP_BOOT) \
	    -acpitable file=$(BUILD)/fakelid.aml -smp 1,maxcpus=4 \
	    -net none -no-reboot -display none

# USB hubs (docs/USB-PLAN.md U6), on the same PC with no PS/2: the keyboard
# and mouse behind a hub, a second hub behind that one with a tablet on it,
# and a USB stick behind the first. The keyboard is typed at; then a second
# keyboard is plugged into the hub while MyOS runs, typed at, and pulled out.
# QEMU's usb-hub is a USB 1.1 hub, so a USB 2 hub's transaction translator
# (the path a laptop's keyboard often takes) is only tried on real hubs.
test-usb-hub: $(USB)
	@rm -f $(BUILD)/usb-hub.log
	@[ ! -f $(OVMF_VARS) ] || cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(for i in $$(seq 60); do sleep 1; \
	    grep -q "Welcome to MyOS" $(BUILD)/usb-hub.log 2>/dev/null && break; done; sleep 1; \
	  tools/qemu-type.sh 'echo HUB-KBD $$((6*7)) ok' | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 1; echo "device_add usb-kbd,bus=xhci.0,port=1.4,id=late"; sleep 3; \
	  tools/qemu-type.sh 'echo LATE-KBD $$((5*5))' | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 1; echo "device_del late"; sleep 3; \
	  tools/qemu-type.sh 'cat /proc/partitions; echo END' | while read k; do echo "$$k"; sleep 0.05; done; \
	  sleep 2; echo quit) \
	  | $(HID_BOOT) \
	    -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
	    -device usb-kbd,bus=xhci.0,port=1.1 -device usb-mouse,bus=xhci.0,port=1.2 \
	    -device usb-hub,bus=xhci.0,port=1.3 -device usb-tablet,bus=xhci.0,port=1.3.1 \
	    -drive if=none,format=raw,file=$(USB),id=stick,snapshot=on \
	    -device usb-storage,bus=xhci.0,port=1.5,drive=stick \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/usb-hub.log -monitor stdio >/dev/null || true
	@perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\r//g' $(BUILD)/usb-hub.log > $(BUILD)/usb-hub.txt
	@L=$(BUILD)/usb-hub.txt; ok=1; \
	  grep -a "usb\|KBD" $$L; \
	  grep -q "^ *usb-hub 1-[0-9]: 8 ports" $$L || { echo "FAIL: the hub found"; ok=0; }; \
	  grep -q "^ *usb-hub 1-[0-9]\.3: 8 ports" $$L || { echo "FAIL: the hub behind the hub"; ok=0; }; \
	  grep -q "^ *usb 1-[0-9]\.1: .* Keyboard" $$L || { echo "FAIL: the keyboard behind the hub"; ok=0; }; \
	  grep -q "^ *usb 1-[0-9]\.2: .* Mouse" $$L || { echo "FAIL: the mouse behind the hub"; ok=0; }; \
	  grep -q "^ *usb 1-[0-9]\.3\.1: .* Tablet" $$L || { echo "FAIL: the tablet two hubs down"; ok=0; }; \
	  grep -q "^ *usb-hid: .* Tablet: a touch screen" $$L || { echo "FAIL: the tablet's driver"; ok=0; }; \
	  grep -q "^ *usb-storage: sd[a-z]: " $$L || { echo "FAIL: the USB stick behind the hub"; ok=0; }; \
	  grep -q "^HUB-KBD 42 ok" $$L || { echo "FAIL: typing on the keyboard behind the hub"; ok=0; }; \
	  grep -q "usb 1-[0-9]\.4: .* Keyboard" $$L || { echo "FAIL: a keyboard plugged in later"; ok=0; }; \
	  grep -q "^LATE-KBD 25" $$L || { echo "FAIL: typing on the keyboard plugged in later"; ok=0; }; \
	  grep -q "usb 1-[0-9]\.4: unplugged" $$L || { echo "FAIL: the late keyboard pulled out"; ok=0; }; \
	  grep -q "^END" $$L || { echo "FAIL: the first keyboard after the second was pulled out"; ok=0; }; \
	  [ $$ok = 1 ] && echo "USB HUB TEST OK ($(ARCH)): two hubs deep, plugged and pulled while running" || exit 1

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
# QEMU's default card is the std VGA, so the Bochs driver switches to
# 1920x1200; zoom-to-fit scales that (or the text screen before it) to the window.
DISPLAY_OPTS := -display cocoa,zoom-to-fit=on

ifeq ($(ARCH),arm64)
run: $(BUILD)/kernel.elf
	chmod +x tools/run-arm64.sh
	tools/run-arm64.sh
else
run: $(IMG) $(USB) $(DATA)
	$(QEMU) $(QEMU_FLAGS) $(DISPLAY_OPTS) -serial stdio
endif

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
else ifeq ($(ARCH),arm64)
test: test-arm64
else
test:
	@$(MAKE) --no-print-directory MODE=test check-selftests
endif

check-selftests: HOSTFWD = ,hostfwd=tcp:127.0.0.1:$(HAIRPIN_PORT)-:7
check-selftests: $(IMG) $(USB)
	$(call make-test-disk,$(DATA),1024)
	@rm -f $(BUILD)/serial.log
	@# Type and click once the self-tests have finished (up to 90 s), not
	@# after a fixed time: the suite grows, and a fixed clock cut it short.
	@(i=0; while [ $$i -lt 900 ] && ! grep -q "^ *Self-tests:" $(BUILD)/serial.log 2>/dev/null; do \
	    sleep 0.1; i=$$((i + 1)); done; sleep 1; \
	  for k in h i ret; do echo "sendkey $$k"; sleep 0.2; done; \
	  echo "mouse_move 10 5"; sleep 0.3; echo "mouse_button 1"; sleep 0.3; \
	  echo "mouse_button 0"; sleep 0.3; \
	  echo "screendump $(BUILD)/screen.ppm"; sleep 0.5; \
	  echo "sendkey f10"; sleep 1; echo quit) \
	  | $(QEMU) $(QEMU_FLAGS) -device edu -display none \
	    -serial file:$(BUILD)/serial.log -monitor stdio >/dev/null
	@cat $(BUILD)/serial.log; echo
	@L=$(BUILD)/serial.log; ok=1; \
	  grep -q "Timer: 1000 Hz (sleep 100 ms = 10[0-9] ticks)" $$L || { echo "FAIL: timer"; ok=0; }; \
	  grep -q "Self-tests: [0-9]* passed, 0 failed" $$L || { echo "FAIL: disk self-tests"; ok=0; }; \
	  if grep -q "wifi sim: iwlwifi would fail" $$L; then echo "FAIL: the Wi-Fi core broke a rule iwlwifi's firmware enforces"; ok=0; fi; \
	  grep -q "fault)> hi" $$L || { echo "FAIL: keyboard echo"; ok=0; }; \
	  grep -q "mouse: PS/2, with a wheel" $$L || { echo "FAIL: PS/2 mouse found"; ok=0; }; \
	  grep -q "mouse: dx=10 dy=-5 wheel=0 buttons=0" $$L || { echo "FAIL: mouse movement"; ok=0; }; \
	  grep -q "mouse: dx=0 dy=0 wheel=0 buttons=1" $$L || { echo "FAIL: mouse click"; ok=0; }; \
	  grep -q "Memory: [0-9]* MiB usable, paging on" $$L || { echo "FAIL: memory init"; ok=0; }; \
	  grep -q "initramfs: [0-9]* KiB at physical" $$L || { echo "FAIL: initramfs loaded"; ok=0; }; \
	  grep -q "Context switch: ping pong ping pong ping pong" $$L || { echo "FAIL: context switch"; ok=0; }; \
	  grep -q "Preemption: spin0 counted [1-9][0-9]*, spin1 counted [1-9]" $$L || { echo "FAIL: preemption"; ok=0; }; \
	  grep -q "brakes ran in slots: 4 9 14 19[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (brakes)"; ok=0; }; \
	  grep -q "main ran in slots: 0 10[[:space:]]*$$" $$L || { echo "FAIL: slot scheduler (main)"; ok=0; }; \
	  grep -q "kconfig: built-in, generation 1: realtime: brakes 4 9 14 19; radio 6 16" $$L \
	    || { echo "FAIL: the built-in kernel config"; ok=0; }; \
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
	  grep -q "net: tcp accept ok" $$L || { echo "FAIL: TCP listen/accept (port $(HAIRPIN_PORT) forwarded in)"; ok=0; }; \
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
	  grep -q "drmtest: all [0-9]* checks passed" $$L || { echo "FAIL: /dev/dri/card0 (Wayland)"; ok=0; }; \
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
                   'cat /etc/hostname' 'cd /etc; pwd' 'uname -sm' 'ip addr' 'ip route' \
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
	  grep -q "inet 10.0.2.15/24 .*eth0" $$L || { echo "FAIL: ip addr (rtnetlink)"; ok=0; }; \
	  grep -q "default via 10.0.2.2 dev eth0" $$L || { echo "FAIL: ip route (rtnetlink)"; ok=0; }; \
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

# --- the live and installer disks ----------------------------------------------
# A repo of their own (myos-live), checked out at live/: its rules are in
# live/live.mk. Without it, its targets say where to get it.
LIVE_TARGETS := live-root live-extra live-theme live-hd live-uefi live-login-initrd \
                test-live-uefi test-usb test-install test-uefi-login test-wifi-desktop \
                utm-live utm-install run-live
ifneq ($(wildcard live/live.mk),)
include live/live.mk
else
$(LIVE_TARGETS):
	@echo "$@: the live disk is built by its own repo. In this folder:"
	@echo "    git clone https://github.com/rw86347/myos-live live"
	@exit 1
endif
