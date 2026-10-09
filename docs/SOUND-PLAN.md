# Sound: the MSI's speakers, jack and microphone

Status (2026-10-07): option A chosen. S1–S3 work in QEMU (`make test-sound`,
both CPUs): the controller, the generic codec driver and Linux's ALSA interface.
Next: S0/S4 on the MSI, then S5 (user space). Not yet: dmix, HDMI (docs/gaps.md).

## The hardware

From `docs/hardware-msi-katana-gf66.md`, checked against Linux 7.2's sources:

| part | what | notes |
|---|---|---|
| controller | Intel Tiger Lake-H **HD Audio**, PCI `8086:43c8` (00:1f.3) | an "Azalia" controller: DMA engines plus a serial link to the codecs |
| codec | **Realtek**, on that link | exact model not recorded yet: the driver reads it at boot (S0) |
| speakers | 2 × 2 W | driven by the codec |
| jack | one 3.5 mm combo jack | headphones, or a headset with a microphone |
| internal mic | not known: on the codec (analog) or on the PCH (digital, DMIC) | decides whether we ever need SOF (below) |
| HDMI audio | on the NVIDIA card, its own HD Audio function `10de:228e` | needs the GPU's HDMI link first: later |

Two facts from Linux that make this easier than it sounds:

- **Linux uses plain HD Audio on this machine, not the DSP.** For Tiger Lake-H,
  `sound/hda/core/intel-dsp-config.c` chooses Intel's DSP path (SOF, with
  the closed `sof-tgl.ri` firmware) only when the laptop has digital mics or
  SoundWire (`FLAG_SOF_ONLY_IF_DMIC_OR_SOUNDWIRE`); otherwise `snd-hda-intel`
  drives it, with no firmware at all.
- **Linux has no fix-up for this laptop.** The Realtek drivers keep a table of
  per-laptop fixes keyed by PCI subsystem; there is none for the Katana's
  `1462:12ea`. Linux runs its codec with the generic setup and the pin
  configuration the BIOS left in the codec. So a spec-based generic codec
  driver should drive it too.

## How HD Audio works

Teaching notes, because the driver is mostly these four ideas:

- **The codec is a graph.** A codec is a small chip of "widgets": pins (the
  speaker, the jack), mixers, volume amplifiers, DACs (digital to analog) and
  ADCs. Each widget lists what it connects to. A driver walks the graph to find
  a path from a DAC to the speaker pin and turns everything on that path on.
  The BIOS writes each pin's "default configuration" (speaker, headphone jack,
  internal mic...), so the driver knows which pin is which.
- **Talking to the codec: two rings.** Commands ("verbs": read a parameter, set
  a volume) go down a ring in RAM called the CORB, and answers come back in
  another, the RIRB. The same idea as xHCI's command and event rings.
- **Sound moves by DMA.** Each playback or capture stream has a list of buffer
  pieces (the BDL). The controller reads the samples from RAM by itself and
  raises an interrupt at the end of each "period" (a few milliseconds of
  sound); it also writes its position to a page in RAM, so finding out where it
  is costs a memory read, not a register read.
- **Jacks report themselves.** Plugging in headphones makes the codec send an
  "unsolicited response" through the RIRB: the driver mutes the speakers and
  unmutes the headphones.

## "As compatible as possible with Linux"

Three layers, and how close each one gets:

1. **What programs see: exactly Linux's.** `/dev/snd/controlC0`,
   `/dev/snd/pcmC0D0p` (playback), `pcmC0D0c` (capture), `/dev/snd/timer`,
   with Linux's ioctl numbers and structures (`include/uapi/sound/asound.h`,
   ALSA protocol 2.0), and `mmap` of the status, control and sound buffers. So
   **alsa-lib, `aplay`, `alsamixer`, PipeWire, Chromium and Firefox run
   unchanged.** As with every other system call, our own header with the same
   layouts.
2. **What a driver sees: Linux's driver API.** Our sound core offers what
   Linux's ALSA core offers drivers: `snd_card_new`, `snd_card_register`,
   `snd_pcm_new`, `struct snd_pcm_ops` (`open`, `close`, `hw_params`,
   `hw_free`, `prepare`, `trigger`, `pointer`), `snd_pcm_period_elapsed`,
   `snd_kcontrol_new` and `snd_ctl_add`, `snd_jack`. The HD Audio parts use
   Linux's names and shapes too (`snd_hdac_bus`, `hda_codec`, verbs,
   `hda_pcm`). This is the Wi-Fi pattern: our core, Linux's interfaces on both
   sides.
3. **The drivers themselves: here the licence rule bites.** Every file of
   Linux's HD Audio stack and its ALSA core is GPL-2.0:

   | Linux 7.2 file | what | licence |
   |---|---|---|
   | `sound/hda/controllers/intel.c` | `snd-hda-intel`, the controller | GPL-2.0-or-later |
   | `sound/hda/core/controller.c`, `stream.c` | the HD Audio bus, streams | GPL-2.0-only |
   | `sound/hda/common/codec.c`, `codecs/generic.c` | the codec core, the generic parser | GPL-2.0-or-later |
   | `sound/hda/codecs/realtek/*.c` | the Realtek codecs | GPL-2.0-or-later |
   | `sound/core/pcm_native.c`, `control.c` | the ALSA core | GPL-2.0-or-later |
   | `sound/soc/sof/intel/*.c` | SOF, the DSP path | GPL-2.0-only **or BSD-3-Clause** |

   The 2026-10-06 decision (`docs/DESIGN.md`: "No GPL-only file goes in") keeps
   all but SOF out, and SOF needs closed firmware plus Linux's ASoC framework,
   which is GPL. So, unlike `iwlwifi`, there is no Linux sound driver we can
   take for this machine.

### A decision for Rodger

- **A. Our own HD Audio drivers, on Linux's API (recommended).** A controller
  driver and a generic codec driver, written from Intel's public *High
  Definition Audio Specification* (rev. 1.0a) against the API in layer 2. Programs
  can't tell the difference (layer 1). About 4,000-5,000 lines. MyOS's licence
  stays a free choice. Because the API is Linux's, the door stays open: if A
  ever falls short on some machine, B is a driver swap, not a redesign.
- **B. Allow GPL for sound and port Linux's drivers.** `snd-hda-intel` plus the
  codec core and the Realtek and HDMI codecs: tens of thousands of lines that
  lean on most of ALSA's internals, so our core would have to reproduce those
  internals closely. Every machine and codec Linux knows comes along. The
  built image becomes GPL-2.0, which reverses the 2026-10-06 decision.
- **C. SOF.** Only if the internal microphone turns out to be digital: SOF is
  the only way to reach DMICs on this chip. Closed firmware, and the ASoC
  framework underneath.

## Steps

Each ends with something visible, in QEMU first. QEMU has a matching controller
and a laptop-like codec: `-device ich9-intel-hda -device hda-micro` (a speaker
and a microphone), and `-audiodev wav` records everything played to a file.
So a test can play a 440 Hz tone and check the recording: a real `make test`
for sound, no ears needed.

- **S0. Facts from the MSI.** The controller's class code (does the BIOS show
  the DSP?), the codec's vendor, device and revision, its pin configurations,
  and whether the ACPI NHLT table lists digital mics. The S1 driver prints
  all of it at boot.
- **S1. The controller.** Reset, the CORB/RIRB rings, MSI interrupts, codec
  discovery. Visible: `hda: codec 0: 10ec:XXXX (Realtek ALCxxx)` on the MSI,
  and QEMU's codec in QEMU.
- **S2. The sound core and the ALSA ABI.** `/dev/snd/*`, the control and PCM
  ioctls (`HW_REFINE`/`HW_PARAMS`, `SW_PARAMS`, `PREPARE`, `START`, `DROP`,
  `DRAIN`, `WRITEI`/`READI`, `SYNC_PTR`, `STATUS`), `mmap`, period interrupts,
  stream DMA. Visible: `aplay tone.wav` in QEMU, and `make test` checks the
  recorded WAV for 440 Hz.
- **S3. The generic codec driver.** Walk the widget graph; paths for the
  speaker, headphones and line out; mixer controls named as Linux names them
  (`Master`, `Speaker`, `Headphone`, `Capture`); EAPD (the speaker amplifier's
  power); jack detection (speakers mute when headphones go in); the
  microphone path. Visible: `alsamixer` in QEMU; playback and recording through
  `hda-micro`.
- **S4. The MSI.** Speakers, headphones and the headset microphone on the real
  machine, plus whatever its Realtek codec needs beyond the spec (S0 will
  show; Linux applies nothing machine-specific here).
- **S5. User space.** `alsa-lib` and `alsa-utils` on the live disk; then a
  sound server so browsers play sound: PipeWire (with `pipewire-pulse` and
  WirePlumber) or PulseAudio. The tray's volume icon becomes real, with the
  volume keys.
- **S6. Later.** HDMI audio (the NVIDIA function), and SOF only if S0 finds
  digital mics.

## Speed and predictability

- **No copies.** The program's buffer *is* the DMA buffer (`mmap`); the
  controller reads samples straight from it.
- **The interrupt does almost nothing.** At the end of a period it reads the
  position from the DMA position page and wakes the waiting thread. No
  copying, no allocation, no codec commands.
- **Low, steady latency.** Short periods (a few ms) with little jitter is
  where a small kernel can beat Linux. Audio is the textbook user of MyOS's
  real-time slots: a sound server's thread can own a slot. Measured with
  `make bench`: the time from the period interrupt to the woken thread running,
  worst case as well as average.
- **No busy-waiting.** Codec commands at boot wait on the RIRB with a time limit;
  at run time (volume changes) they use its interrupt.

## What the rest of the kernel must have

- Mapping kernel DMA pages into a program (`VM_PFNMAP`, as for `/dev/fb0`): there.
- MSI interrupts, DMA memory below 4 GiB: there.
- `poll` on the PCM devices: part of S2.
- For PipeWire (S5): `timerfd` (work list B16), plus `eventfd`, `memfd` and
  passing fds over Unix sockets, which are there. PulseAudio needs less.

## Questions for Rodger

- A, B or C? (A is recommended; B reverses the licence decision.)
- PipeWire or PulseAudio for S5? (PipeWire is Alpine's default and also handles
  screen sharing later; PulseAudio is smaller and needs fewer system calls.)
