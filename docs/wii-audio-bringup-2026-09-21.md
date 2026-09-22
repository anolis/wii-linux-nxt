# Wii audio DMA bring-up — 2026-09-21

Experimental `CONFIG_SND_GAMECUBE` / `snd-gcn-ai` restores ALSA stereo
S16_BE playback at 32 and 48 kHz. It binds the current `hollywood-ai` or
`flipper-ai` node and claims the separate DSP register resource. No DTB
change is required for the installed Wii. This has not been tested on a
GameCube. The Wii defconfig and boot kernel are deliberately unchanged
pending physical sound verification and further timing tests.

## Implementation

- ALSA-managed coherent DMA buffers use the DMA address, including Wii
  MEM2 addresses, rather than a CPU virtual pointer. Wii has a 29-bit DMA
  mask; GameCube has 26 bits. Periods are aligned to 32 bytes.
- AID signals the start/latch of a buffer. The first interrupt does not
  complete a period. Subsequent interrupts queue the following ring period
  without stopping DMA. The remaining-block counter supplies subperiod
  position, with a pending-interrupt check across the counter read.
- STOP immediately disables DMA and its interrupt. ALSA's sync_irq and
  explicit close/remove synchronization protect buffer/substream lifetime.
  The period notification runs outside the driver lock.
- `/proc/asound/card0/ai` exposes counters and DMA registers. No mixer,
  capture, hardware pause, or system suspend/resume is implemented.
- AI and DSP are separate DT devices. The current DSP resource is otherwise
  unclaimed; a future independent DSP driver would need shared ownership
  coordination. The AV encoder configuration is untouched.

References: the legacy GameCube Linux `sound/ppc/gcn-ai.c`,
[libogc audio](https://github.com/devkitPro/libogc/blob/master/libogc/audio.c),
[Dolphin DSP](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSP.cpp),
and [ALSA driver guide](https://www.kernel.org/doc/html/latest/sound/kernel-api/writing-an-alsa-driver.html).

## Hardware evidence

Wii at 10.3.10.59, running `6.18.40-wii+`, built from the existing matched
kernel build directory `/media/anolis/dev/wiidesk-os-build/kernel`.
The module was compiled externally with `W=1`, without warnings, and passed
checkpatch. Loaded module SHA-256:
`2e732c6adfbb59fabb7f75f83425e5904723834b2a9418ec5e9b56ddabf4802e`.
Staging: `/var/tmp/wii-audio-driver-20260921` on the Wii.

`tools/wii-gcn-audio-test.c` accepts:

```
wii-gcn-audio-test DEVICE RATE SECONDS PERIOD_FRAMES PERIODS silence|tone [rw|mmap]
```

It uses nonblocking ALSA, fails on underruns instead of recovering them,
bounds playback/drain time, and checks elapsed duration within 5% plus
100 ms. Tone mode is optional and was not used. Nonblocking drain must
check the PCM state: this kernel returns EAGAIN even after reaching SETUP.
The original apparent drain stalls were a test bug, not stuck hardware.
The same correction was needed in WiiDesk's audio worker.

Silent results after fixing the test:

| Coverage | Result |
| --- | --- |
| 32/48 kHz, 512/1024/2048-frame periods, four periods, 3 s each | 6/6 pass |
| 32/48 kHz, 480-frame periods, four periods, five 3 s repetitions each | 10/10 pass |
| 32 kHz, 1024-frame periods, eight periods, 30 s | Pass, 30.470 s |
| 48 kHz, same buffer, 30 s | Pass, 30.020 s |
| WiiDesk WAV/MP3 through default, muted, pause/seek/resume/drain | Pass, no XRUN/ERROR |
| MMAP at 32/48 kHz, 1024-frame periods, eight periods, 5 s each | 2/2 pass, 5.026/5.015 s |
| SIGKILL during playback | DMA stopped; starts/stops both 27 |
| Module unload/reload, followed by 3 s playback | Pass, 3.017 s |

Player evidence: `/var/tmp/wiidesk-audio-test.HxNOHX`. The worker runs as
`wii`; no root audio workaround or permanent silent ALSA configuration.

One earlier 48 kHz run with a 40 ms buffer underrran after 19 periods.
Later repetitions passed, but the initial failure is not dismissed:
scheduling under UI/network load and pointer/timing behavior still need
characterization. The 32 kHz long run was 1.6% slow; the present tolerance
accepts it, but clock accuracy/interrupt latency are not conclusively
validated by elapsed wall time alone.

## Remaining gates

1. Listen for actual stereo output, channel order, rate/pitch, clicks and
   pause/seek behavior. The user was away; all current tests were silent.
2. Test UI/network load and tighter timing/interrupt accounting; investigate
   the small-buffer underrun rather than claiming a zero failure rate.
3. Verify AV encoder audio routing if sound is absent. Do not reset the
   already-working video path speculatively.
4. Only then enable the module in the image configuration, install it with
   depmod, verify cold-boot autoload, and refresh the WiiDesk OS image.
