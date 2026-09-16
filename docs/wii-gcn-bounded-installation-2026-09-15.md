# Persistent bounded scaling installation — September 15, 2026

Installed and activated on the Wii at 10.3.10.59, kernel `6.18.40-wii+`, with
explicit user authorization. Repository defaults remain unchanged; this is a
persistent configuration of the tested device. The same WiiDesk process (790)
is active on tty7 with the accelerator loaded. No reboot was performed, so the
cold-boot path has not yet been observed.

## Installed files

- `/lib/modules/6.18.40-wii+/kernel/drivers/video/fbdev/gcn-gx.ko`:
  exact qualified quiet/cached module, SHA256
  `6b0b956f7a02c6e05b22351b86e82b928345affa95cad6bac39b36b071229f64`.
- `/etc/modprobe.d/gcn-gx-bounded.conf`: new file with the following settings.
- `/etc/init.d/wiidesk`: adds `modprobe gcn_gx || return 1` after the VI ownership
  handoff and before launching WiiDesk. Its previous startup sequence unloaded
  the accelerator; this reload makes it available after the handoff. Existing
  desktop process and display ownership were preserved during installation.

```text
options gcn_gx scale_bounded_final=1 scale_bounded_horizontal=1 scale_bounded_coord_cache=1 scale_bounded_log=0 scale_bounded_batch_quads=600 scale_identity_quad=1
```

`depmod -a 6.18.40-wii+` completed. A dry-run modprobe resolved the installed
module with this preset, then a real `modprobe gcn_gx` loaded it. All six sysfs
parameters matched: Y, Y, Y, N, 600, Y respectively. No kernel image, bootloader,
module-load list or repository driver default was changed.

## Verification

The installed provider passed three fully checked display smoke tests, 30 frames
in each of RGB565 scaling, XRGB8888 scaling and native tiled XRGB8888: 90 frames,
27648000 pixel comparisons, no pixel mismatch or failed flip. Every client
restored its CRTC and recovered 524288 free MEM1 bytes. The module remains loaded
and registered as the scanout accelerator. Boot UUID and printk are unchanged;
WiiDesk resumed on tty7. Shell syntax checks passed for the installation,
rollback and modified startup scripts. This deployment uses the already
qualified binary; it does not change the limits of the prior fault investigation.

Complete local evidence and payload:
`/media/anolis/dev/wii-gcn-install-20260915`, including `install.log`,
`final-state.txt`, original/new startup scripts, installer, configuration,
qualified module, smoke client, rollback backup, and `ARCHIVE-SHA256SUMS`.
The local rollback tar was independently checked against the original module
hash and the saved original startup script. Staged device files are under
`/var/tmp/wii-gx-install-20260915`.

## Rollback

The on-device backup is `/root/wii-gx-rollback-20260915`. It contains the original
module, original WiiDesk startup script, pre-install configuration tar, checksums
and executable rollback script. The original module SHA256 is
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.

As root on the Wii, with accelerator clients stopped:

```sh
sh /root/wii-gx-rollback-20260915/rollback.sh
```

This unloads the new accelerator, restores the original module and startup
script, moves the newly added modprobe preset into the backup directory,
regenerates dependencies, and resumes tty7. It leaves the old module unloaded,
matching the state before installation. It does not restart WiiDesk. Both the
backup and its off-device copy are retained. The installer had an automatic
rollback trap for install/load/smoke-test failures; it was not triggered.
