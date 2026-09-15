# Bounded scaling candidate — September 15, 2026

Proposed loading preset for the next controlled application trial:

```text
scale_bounded_final=1 scale_bounded_horizontal=1 scale_bounded_coord_cache=1 scale_bounded_log=0 scale_bounded_batch_quads=600
```

Keep `scale_identity_quad=1` (the existing default). The temporary module tested
is `/media/anolis/dev/wii-gcn-bounded-quiet-module.ko`, SHA256
`6b0b956f7a02c6e05b22351b86e82b928345affa95cad6bac39b36b071229f64`.
This document is a reviewable preset, not a persistent modprobe configuration or
a source-default change. Diagnostic cases must retain their explicit settings;
a future global-default patch must also make baseline test settings explicit.

The bounded geometry and cached coordinates passed the 1000-call workload-family
qualification, mixed-axis batch-boundary repeats, and quiet-mode tests described
in [the investigation report](wii-gcn-fault-investigation-2026-09-12.md).
Real KMS presentation subsequently passed 120 and 300 frames in each of RGB565
scaling, XRGB8888 scaling and native tiled XRGB8888 modes: 1260 frames and
387072000 destination pixel checks. Every case restored its CRTC and recovered
524288 free MEM1 bytes. The desktop process survived and resumed on tty7.
VLC samples showed the expected pattern/moving marker and restored greeter;
the user also reported that the display looked good.

Limits: the pixel oracle runs on every frame, so recorded cadence is not a
normal application FPS benchmark. Native tiled mode uses the existing identity
shortcut. Sampled video observations cannot prove absence of every transient
artifact. WiiDesk's greeter/handoff was observed; authenticated desktop workflows
were not exercised, and WiiDesk's CPU dumb-buffer renderer does not itself test
this scaling path. The original fault mechanism remains unexplained: this is a
validated workaround for the tested workloads, not a universal GPU fix.

For a temporary trial, use the existing render-cycle/matrix cleanup rather than
installing the module. Cleanup unloads the temporary `gcn_gx`, restores CPU
scanout and the original printk value. If tty1 is visible afterward, the running
WiiDesk session can be resumed with `busybox chvt 7`; verify its existing PID and
that tty7 is active. Do not restart the desktop or replace the installed module
just to perform that handoff.

The installed rollback provider has remained untouched:
`/lib/modules/6.18.40-wii+/kernel/drivers/video/fbdev/gcn-gx.ko`, SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Any later persistent installation must retain that exact binary and its prior
configuration as the rollback point. No persistent installation was performed
for this validation.
