#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# check-asound.sh — prove include/sound/asound.h has Linux's ALSA layouts.
#
# For each CPU, compile two lists of numbers — sizes, field offsets and ioctl
# numbers — once from Linux's include/uapi/sound/asound.h (seen through the
# live disk's musl headers, the way Alpine builds alsa-lib) and once from
# ours, then compare the bytes. Needs build/ext/linux-*.tar.xz and the live
# sysroots (make live-hd fetches them).
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
want='*/include/uapi/sound/asound.h'      # GNU tar wants --wildcards; macOS's refuses it
tar -xJf build/ext/linux-*.tar.xz -C "$tmp" --strip-components=4 --wildcards "$want" 2>/dev/null ||
    tar -xJf build/ext/linux-*.tar.xz -C "$tmp" --strip-components=4 "$want"
mkdir -p "$tmp/stub/linux" "$tmp/stub/asm"
printf '%s\n' '#include <stdint.h>' '#include <sys/ioctl.h>' '#define __bitwise' '#define __user' '#define __packed __attribute__((packed))' \
    'typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32;' \
    'typedef uint64_t __u64; typedef int32_t __s32; typedef int64_t __s64;' \
    'typedef long __kernel_off_t; typedef int __kernel_pid_t;' > "$tmp/stub/linux/types.h"
printf '%s\n' '#define __LITTLE_ENDIAN 1234' > "$tmp/stub/asm/byteorder.h"

# The same list for both headers: L(x) puts one number in.
cat > "$tmp/list.h" <<'EOF'
#include <stddef.h>
#define L(x) (unsigned long)(x),
#define O(s, f) L(offsetof(struct s, f))
const unsigned long vals[] = {
    L(sizeof(struct snd_pcm_info)) O(snd_pcm_info, name) O(snd_pcm_info, subdevices_count)
    L(sizeof(struct snd_interval)) L(sizeof(struct snd_mask))
    L(sizeof(struct snd_pcm_hw_params)) O(snd_pcm_hw_params, intervals) O(snd_pcm_hw_params, rmask)
    O(snd_pcm_hw_params, fifo_size) O(snd_pcm_hw_params, sync)
    L(sizeof(struct snd_pcm_sw_params)) O(snd_pcm_sw_params, avail_min)
    O(snd_pcm_sw_params, start_threshold) O(snd_pcm_sw_params, boundary) O(snd_pcm_sw_params, proto)
    L(sizeof(struct snd_pcm_channel_info)) O(snd_pcm_channel_info, offset) O(snd_pcm_channel_info, first)
    L(sizeof(struct snd_pcm_status)) O(snd_pcm_status, trigger_tstamp) O(snd_pcm_status, tstamp)
    O(snd_pcm_status, appl_ptr) O(snd_pcm_status, hw_ptr) O(snd_pcm_status, delay)
    O(snd_pcm_status, avail) O(snd_pcm_status, suspended_state) O(snd_pcm_status, audio_tstamp)
    O(snd_pcm_status, audio_tstamp_accuracy)
    L(sizeof(struct snd_pcm_sync_ptr)) O(snd_pcm_sync_ptr, s) O(snd_pcm_sync_ptr, c)
    O(snd_pcm_sync_ptr, s.status.hw_ptr) O(snd_pcm_sync_ptr, s.status.tstamp)
    O(snd_pcm_sync_ptr, s.status.suspended_state) O(snd_pcm_sync_ptr, c.control.avail_min)
    L(sizeof(struct snd_xferi)) O(snd_xferi, buf) O(snd_xferi, frames)
    L(sizeof(struct snd_ctl_card_info)) O(snd_ctl_card_info, mixername)
    L(sizeof(struct snd_ctl_elem_id)) O(snd_ctl_elem_id, name) O(snd_ctl_elem_id, index)
    L(sizeof(struct snd_ctl_elem_list)) O(snd_ctl_elem_list, pids)
    L(sizeof(struct snd_ctl_elem_info)) O(snd_ctl_elem_info, type) O(snd_ctl_elem_info, value)
    O(snd_ctl_elem_info, value.integer.max) O(snd_ctl_elem_info, reserved)
    L(sizeof(struct snd_ctl_elem_value)) O(snd_ctl_elem_value, value)
    L(sizeof(struct snd_ctl_event)) O(snd_ctl_event, data.elem.id)
    L(SNDRV_PCM_VERSION) L(SNDRV_CTL_VERSION)
    L(SNDRV_PCM_IOCTL_PVERSION) L(SNDRV_PCM_IOCTL_INFO) L(SNDRV_PCM_IOCTL_TSTAMP)
    L(SNDRV_PCM_IOCTL_TTSTAMP) L(SNDRV_PCM_IOCTL_USER_PVERSION) L(SNDRV_PCM_IOCTL_HW_REFINE)
    L(SNDRV_PCM_IOCTL_HW_PARAMS) L(SNDRV_PCM_IOCTL_HW_FREE) L(SNDRV_PCM_IOCTL_SW_PARAMS)
    L(SNDRV_PCM_IOCTL_STATUS) L(SNDRV_PCM_IOCTL_DELAY) L(SNDRV_PCM_IOCTL_HWSYNC)
    L(SNDRV_PCM_IOCTL_SYNC_PTR) L(SNDRV_PCM_IOCTL_STATUS_EXT) L(SNDRV_PCM_IOCTL_CHANNEL_INFO)
    L(SNDRV_PCM_IOCTL_PREPARE) L(SNDRV_PCM_IOCTL_RESET) L(SNDRV_PCM_IOCTL_START)
    L(SNDRV_PCM_IOCTL_DROP) L(SNDRV_PCM_IOCTL_DRAIN) L(SNDRV_PCM_IOCTL_PAUSE)
    L(SNDRV_PCM_IOCTL_REWIND) L(SNDRV_PCM_IOCTL_RESUME) L(SNDRV_PCM_IOCTL_XRUN)
    L(SNDRV_PCM_IOCTL_FORWARD) L(SNDRV_PCM_IOCTL_WRITEI_FRAMES) L(SNDRV_PCM_IOCTL_READI_FRAMES)
    L(SNDRV_PCM_IOCTL_LINK) L(SNDRV_PCM_IOCTL_UNLINK)
    L(SNDRV_CTL_IOCTL_PVERSION) L(SNDRV_CTL_IOCTL_CARD_INFO) L(SNDRV_CTL_IOCTL_ELEM_LIST)
    L(SNDRV_CTL_IOCTL_ELEM_INFO) L(SNDRV_CTL_IOCTL_ELEM_READ) L(SNDRV_CTL_IOCTL_ELEM_WRITE)
    L(SNDRV_CTL_IOCTL_ELEM_LOCK) L(SNDRV_CTL_IOCTL_ELEM_UNLOCK) L(SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS)
    L(SNDRV_CTL_IOCTL_TLV_READ) L(SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE) L(SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE)
    L(SNDRV_CTL_IOCTL_PCM_INFO) L(SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE)
    L(SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE) L(SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE)
    L(SNDRV_CTL_IOCTL_POWER) L(SNDRV_CTL_IOCTL_POWER_STATE)
};
EOF
printf '#include "asound.h"\n#include "list.h"\n' > "$tmp/linux.c"
printf '#include <sound/asound.h>\n#include "list.h"\n' > "$tmp/ours.c"

fail=0
for cpu in i386 x86_64; do
    if [ $cpu = i386 ]; then cc=i686-elf-gcc sys=build/live/sysroot def=-DARCH_I386
    else cc=x86_64-elf-gcc sys=build/live/sysroot-x86_64 def=-DARCH_X86_64; fi
    $cc -O0 -c -w -nostdinc -D__linux__ -I"$tmp/stub" -isystem $sys/usr/include \
        -isystem "$($cc -print-file-name=include)" -I"$tmp" "$tmp/linux.c" -o "$tmp/l.o"
    $cc -O0 -c -w -ffreestanding -nostdinc -Iinclude $def -isystem "$($cc -print-file-name=include)" \
        -I"$tmp" "$tmp/ours.c" -o "$tmp/o.o"
    objcopy=${cc%gcc}objcopy
    $objcopy -O binary -j .rodata "$tmp/l.o" "$tmp/l.bin"
    $objcopy -O binary -j .rodata "$tmp/o.o" "$tmp/o.bin"
    if cmp -s "$tmp/l.bin" "$tmp/o.bin"; then
        echo "asound.h: $cpu matches Linux ($(wc -c < "$tmp/l.bin" | tr -d ' ') bytes of sizes, offsets and ioctls)"
    else
        echo "asound.h: $cpu DIFFERS from Linux (entry, Linux, ours):"
        python3 - "$tmp/l.bin" "$tmp/o.bin" $cpu <<'EOF'
import sys, struct
w = 4 if sys.argv[3] == 'i386' else 8
a, b = (open(p, 'rb').read() for p in sys.argv[1:3])
f = '<%d%s' % (len(a) // w, 'I' if w == 4 else 'Q')
for i, (x, y) in enumerate(zip(struct.unpack(f, a), struct.unpack(f, b))):
    if x != y: print('  %3d  %#x  %#x' % (i, x, y))
EOF
        fail=1
    fi
done
exit $fail
