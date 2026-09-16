# IceWM/X11 hardware trial — September 15, 2026

IceWM 4.0.0 and an xterm are running as user `wii` on Xorg 1.21.1.24, display
`:0`, tty8. The Wii's existing WiiDesk process (790) remains available on tty7.
WiiDesk remains the boot desktop; this is a manual comparison session, not a
replacement startup policy. No kernel or installed accelerator change was needed.

## Package management constraint

**Do not run apt or apt-cache on the Wii, including apt-get simulations.**
An initial `apt-get -s` in this trial caused severe memory pressure and SSH
handshake stalls. It was stopped with `busybox killall -KILL apt-get`, with no
package mutations from the simulation. Even a read-only resolver is too costly
for this machine. Use host-side resolution/download and `dpkg` on the Wii.

The host used the existing pinned Debian Ports snapshot
`20260813T000000Z`, powerpc architecture, a private apt configuration/list/cache,
and a copied `/var/lib/dpkg/status` from the Wii. The final transaction installed
158 new packages, with zero upgrades/removals, and no recommends. Approximately
105 MB of archives and 423 MB installed space were required. Primary requested
packages: `xserver-xorg-core`, `xserver-xorg-input-libinput`, `xinit`, `xterm`,
`icewm`, and `xfonts-base`. The resolver pulls in graphics-library dependencies
even though this trial does not enable OpenGL rendering.

All .deb SHA256 hashes matched the snapshot index on the host and were checked
again on the Wii. Packages were transferred to disk-backed `/var/tmp`, then
installed using `dpkg --unpack debs/*.deb` and `dpkg --configure -a`, with
`DEBIAN_FRONTEND=noninteractive` and `DPKG_DEB_THREADS_MAX=1`. `dpkg --audit`
was empty afterward. Transfer and installation were slow; the large LLVM
package expands to roughly 147 MiB, although its XZ decoder needs only 9 MiB.

## Display and session setup

The explicit modesetting configuration uses `/dev/dri/card0`, 640x480, depth
16/bpp16, software cursor, ShadowFB enabled, and `AccelMethod "none"`; the GLX
module is disabled. Xorg confirmed damage tracking and the selected mode.
These options follow the [Debian modesetting manual](https://manpages.debian.org/unstable/xserver-xorg-core/modesetting.4.en.html).
This tests standard KMS/shadow-buffer display. It does not establish X11 drawing
acceleration through the custom GX rendering ioctls. The installed bounded GX
scanout provider remains loaded with its qualified settings.

Xorg runs as root for VT/device access, with `-nolisten tcp` and an X authority
cookie. IceWM and xterm run as the existing unprivileged `wii` account. Their
private state is under `/home/wii/.local/state/wii-x11-trial`; the cookie was not
copied into evidence. IceWM is launched directly, without a compositor or its
optional session helpers. Move/resize use outlines; CPU/network/mailbox taskbar
monitors are disabled. No display manager or boot service was added.

Manual trial files are in `/var/tmp/wii-icewm-trial-20260915` on the Wii:
`xorg-wii.conf`, `start-xorg.sh`, `start-terminal.sh`, `start-icewm.sh`, and
`stop-trial.sh`. With no existing X trial, run the three start scripts in that
order, checking Xorg startup before starting clients. Do not start duplicate
sessions. `stop-trial.sh` checks the saved Xorg PID, terminates the trial server,
and selects tty7; session termination itself has not been exercised in this run.

To select either already running desktop as root:

```sh
busybox chvt 7  # WiiDesk
busybox chvt 8  # IceWM
```

## Checks and limitations

- Plain xterm displayed correctly before starting IceWM, verified through VLC.
- IceWM decorated and managed the terminal, with its panel visible.
- `icesh` moved and resized the terminal; geometry changed from
  `460x316+16+44` to `364x238+104+104`, with a matching VLC capture.
- Closing the terminal exited its process; a new terminal was launched and
  managed correctly. An immediate post-close diagnostic query raced destruction
  and printed BadWindow messages; later queries were clean.
- tty8 -> tty7 -> tty8 worked, with both desktops visually checked and both
  sessions preserved. IceWM was left active on tty8.
- Xorg detected the Dell USB keyboard via libinput. No USB mouse was connected.
  Physical typing, mouse interaction, prolonged redraw testing and general
  application performance remain untested. XKB logged nonfatal symbol warnings.
- `MemAvailable` ranged around 20–22 MiB with both desktops present. One
  post-handoff sample showed Xorg RSS/swap 8300/4908 KiB, IceWM 2552/1992 KiB,
  and xterm 3112/1920 KiB. These are samples with shared pages and prior swap
  activity, not a clean aggregate working-set measurement. `smaps_rollup` is
  unavailable in this kernel. No responsiveness/FPS claim is made.
- The qualified installed module hash, modprobe preset and WiiDesk init-script
  hashes remained unchanged; boot UUID and printk also remained unchanged.

Local evidence, scripts, host resolver configuration, snapshots, verified debs
and package logs are archived at
`/media/anolis/dev/wii-icewm-trial-20260915`, with recursive `SHA256SUMS`.
The trial demonstrates that an existing X11 desktop can run on the current
standard display stack. Next useful work is hands-on keyboard/mouse interaction
and a small application workload before any decision to replace WiiDesk.
