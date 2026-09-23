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
wii-gcn-audio-test DEVICE RATE SECONDS PERIOD_FRAMES PERIODS silence|tone|left|right [rw|mmap [VOLUME_PERCENT]]
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

## Audible follow-up — September 22

The user confirmed audible output from a five-second 48 kHz stereo tone
at the requested 10% digital amplitude. It completed in 5.036 seconds
without underruns. This confirms sound reaches the user's listening path;
it does not by itself establish channel order or eliminate intermittent faults.

The test client now accepts separate left/right tones and an optional
0–100% amplitude (the existing default remains 1200/32767). Two-second
left and right tests at 10% completed in 2.025 and 2.012 seconds without
underruns. A full-scale stereo WAV played through WiiDesk's worker with
`V 10` queued before playback; pause, resume, seek to 6000 ms, and natural
completion all reported the expected states with no XRUN/ERROR. The user
confirmed correct left/right channels and clean audible pause/resume.
The subsequent 32 kHz stereo tone at 10% completed in 3.022 seconds without
underruns; the user confirmed clean sound with no crackling or dropouts.

## Load testing — September 22

`tools/wii-gcn-audio-sweep.sh CLIENT READ_FIXTURE [RESULT_DIRECTORY]` runs
12 silent eight-second cases as the ordinary audio user: 32/48 kHz,
480-frame/four-period and 1024-frame/eight-period rings, each at idle,
under CPU load (`sha256sum /dev/zero`) and under repeated direct reads of
an existing file. Workloads are bounded and cleaned up; output is a compact
progress count with per-case logs and a TSV summary. Direct-read support is
checked before starting. No caches are dropped and no storage is overwritten.

First sweep `/var/tmp/wii-audio-sweep.f7H7cJ`: 11/12 passed. The 32 kHz
60 ms ring underrran under CPU load; all idle, larger-ring, and disk-load
cases passed. This is a deliberately mixed test matrix, not an estimate
of a normal-use failure probability.

WiiDesk's installed 100 ms worker also reported an XRUN under CPU load:
`/var/tmp/wii-audio-driver-20260921/cpu-worker-48k.log` and the MP3 control
test `/var/tmp/wiidesk-audio-test.2PEpBR`. A staged 160 ms/140 ms producer-lead
candidate still underrran in one of five eight-second 48 kHz runs
(`buffer160/cpu-48k-2.log`). It was not deployed; source defaults remain
100 ms. Buffer enlargement alone is not a validated fix.

The experimental driver now exposes per-prepare nominal period time,
maximum interrupt gap, gaps over 1.5 periods, and backward pointer-report
counts in its existing proc entry. These diagnostics do not change DMA
or pointer behavior; they distinguish evidence of IRQ delay from position
accounting faults. A long IRQ gap is not by itself proof of a lost period.

The diagnostic module (`timing/snd-gcn-ai.ko`, SHA-256
`cdffba21a68e2aa2a9f00c2bd19442ba729b4e844ed9059edc7ccecc4c0c3251`)
passed four ten-second CPU-loaded runs with no pointer rewinds. Three had
maximum interrupt gaps of 31.2–38.8 ms against a 15 ms period. A subsequent
run underrran after 191 completed periods, recording a 51.048 ms maximum
gap and nine gaps above 1.5 periods, still with zero pointer rewinds
(`timing/cpu-long.log`). This supports investigating interrupt latency;
it does not yet identify which kernel path caused the delay.

A traced 60-second repeat passed, with a 35.257 ms maximum gap and no
pointer rewinds. The 1 MiB rolling IRQ trace retained only the final
18.4 seconds, which did not include the long gap. In that retained window,
the largest audio IRQ gap was 15.058 ms and all traced handlers finished
within 0.110 ms. Do not attribute the earlier delay to MMC, graphics, or
audio IRQ handlers based on this trace. Local trace copy:
`/media/anolis/dev/wii-audio-module/irq-trace-20260922.txt.gz`.

### Captured source of an audio interrupt delay

The corrected trigger waits for `running=1` before checking `late_irqs`,
freezes tracing on the first late interval, and waits for playback to end
before saving the trace. It retained all 15,518 events (no overwrite).
The 30-second run passed in 30.023 seconds but captured this sequence:

| Timestamp (seconds) | Event |
| --- | --- |
| 27261.209005 | gcn-ai IRQ entry |
| 27261.211307 | mmc0 IRQ entry, interrupted task jbd2/mmcblk0p2 |
| 27261.242366 | mmc0 IRQ exit, after 31.059 ms |
| 27261.242379 | gcn-ai IRQ entry, after a 33.374 ms audio gap |

Trace flags show interrupts disabled throughout that mmc0 handler. The
largest mmc0 handler in this capture took 31.120 ms. Thus SD-card hard-IRQ
latency is a demonstrated source of audio delays, although this passing
run cannot prove that every earlier underrun had exactly the same cause.
The trace is `/media/anolis/dev/wii-audio-module/irq-triggered-v2-20260922.txt.gz`
and `timing/triggered-v2-trace.txt.gz` on the Wii.

Source inspection explains a plausible mechanism: Hollywood uses PIO,
`sdhci_transfer_pio()` drains all currently available blocks in the IRQ,
and the Hollywood accessor delays five microseconds after every write,
including 32-bit FIFO writes. The trace identifies the handler but does
not separately instrument that internal loop. Next: bound Hollywood
SD-card PIO work (for example through bounded requests), preserve the
required register delays, and measure both worst-case IRQ time and storage
throughput on a recoverable test kernel before adopting the change.

The first trigger attempt was invalid: stale counters stopped it before
audio started. Its 278 ms maximum must not be used for attribution because
it also saved tracing data during playback. It is superseded by v2.
Tracing and synthetic load processes have been stopped; the trace buffer
was reduced and tracefs unmounted. No boot configuration or player-buffer
default was changed.

## Request bounding and installed mitigation — September 22–23

The block queue's existing `max_sectors_kb` control provided a reversible
way to test smaller requests without replacing the running boot kernel.
`tools/wii-gcn-audio-storage-test.sh CLIENT [BASELINE_KIB]` compares the
selected baseline (default: current limit) with 4 KiB, restores the previous
limit, and checks the test file's SHA-256. The return status requires the
4 KiB audio case to pass; a failing baseline is retained as a control.

During direct writes/reads of a dedicated four-MiB file and 20 seconds of
48 kHz silent playback with a 40 ms ring:

| Request cap | Audio | Maximum IRQ gap | Write/read throughput |
| --- | --- | --- | --- |
| 512 KiB | Underrun | 92.652 ms | 678 kB/s / 17.6 MB/s |
| 8 KiB | Underrun | 19.716 ms | 617 kB/s / 10.0 MB/s |
| 4 KiB, run 1 | Pass | 15.189 ms | 538 kB/s / 6.3 MB/s |
| 4 KiB, run 2 | Pass | 15.147 ms | 516 kB/s / 6.4 MB/s |

All four file hashes matched. Evidence: `/var/tmp/wii-audio-storage.iQjIL6`
and `/var/tmp/wii-audio-storage.kHSuap`. This demonstrates a latency/throughput
tradeoff on the tested card, not a general performance guarantee.

The installed WiiDesk OS udev rule now applies the 4 KiB cap only to whole
MMC disks whose parent driver is `sdhci-hlwd`. Syntax verification and a
targeted udev change event passed, with the queue reporting `4`. Source and
rollback details are in `wiidesk-os/docs/sd-audio-latency.md`. The required
register delays remain unchanged. A future write-side kernel optimization
may retain more read throughput; this policy is a mitigation, not that fix.

The audio driver now preallocates 64 KiB rather than 32 KiB. The player
requests a 250 ms ring (48,000 bytes at 48 kHz), fills available PCM space
before waiting, and retains bounded producer lead. This adds 32 KiB of
fixed DMA allocation. Wii defconfig enables `CONFIG_SND_GAMECUBE=m`; an
isolated defconfig generation confirmed both SND_PPC and SND_GAMECUBE.

### Load-test methodology correction

Early combined CPU/storage/player tests redirected frequent player status
updates to the SD card under test. That synchronous log sink can itself
block the worker; the desktop instead uses a status pipe. Those results
cannot isolate the effect of changing playback buffer size. The final
`tools/wii-gcn-audio-player-load-test.sh` keeps live logs in tmpfs, persists
them only after stopping load, and holds worker stdin open to natural
completion. It accepts `WIIDESK_TEST_WORKER` and `WIIDESK_AUDIO_LOAD_RUNS=0..5`
(zero repeats only the control checks). Its audio source remains on SD;
the separate short WAV/MP3 control fixtures/logs are copied into tmpfs.

With corrected logging, five eight-second 48 kHz WAV runs passed under
simultaneous CPU hashing and repeated direct SD writes/reads: no XRUN/ERROR,
all natural completions (`/var/tmp/wii-audio-player-load.JD1eV7`). The first
run recorded a 588 ms maximum decode/write gap without an underrun; these
metrics include startup before PCM playback begins.
The control test initially raced worker exit after a successful completion;
after correcting its wait, WAV/MP3 pause/seek/resume/end and missing-device
checks passed under both loads (`/var/tmp/wii-audio-player-load.3d5nEq`,
details `/dev/shm/wiidesk-audio-test.NtnNSZ`). These short tests do not prove
zero failures under arbitrary load.

The matching module is installed in `/lib/modules/6.18.40-wii+/extra` with
depmod, and reload through modprobe passed. Both 32/48 kHz MMAP tests using
the full 64 KiB ring passed in 3.013/3.032 seconds, including allocation in
MEM1 after earlier MEM2 testing. No late IRQ or backward pointer report
occurred in the final 48 kHz check. Module SHA-256:
`fd37b07beecb46c720a27ad1534be8411288bb8028bd05b54045fa5bd6e780e0`.
The updated player is installed with its predecessor backed up. The running
boot kernel is unchanged; cold-boot autoload and full OS image refresh remain.
