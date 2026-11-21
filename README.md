# DS4L

DualSense for Linux: the parts of the PS5 controller that Linux doesn't
drive on its own.

The kernel already handles a DualSense as a gamepad: buttons, sticks,
touchpad, motion sensors, battery, lightbar and rumble. Wired, its speaker,
headset jack and microphone also show up as an ordinary sound card. What's
missing is everything else:

* **Over Bluetooth:** the speaker, the HD haptics (the voice-coil rumble
  games use on a PS5) and the microphone. DS4L adds them as a normal
  playback and capture device in your audio settings.
* **Bluetooth or USB:** adaptive triggers, plus lightbar, player LEDs,
  rumble, volume and mute, from the `ds4l` command or from other programs.

You don't need DS4L if you only use the controller as a plain gamepad.

**Minecraft players:** if you came here from
[DS4C](https://github.com/Mac898/DS4C), this is the Linux side it links
to. Install DS4L as below, then follow DS4C's README.

## Status

Early, version 0.1. Developed and tested on one Ubuntu-based desktop
(kernel 7.1, PipeWire with WirePlumber) with a DualSense over Bluetooth
(Intel 8260 and AX200 adapters) and over USB. The DualSense Edge is
recognised but untested. Other distros should work; the package lists
below are untested. The kernel module is compile-tested against Linux
6.12, 7.0, 7.1 and 7.2.

## Requirements

* Linux 6.12 or newer (`uname -r`).
* PipeWire with WirePlumber, for the audio devices.
* A systemd user session, for the background service (or run `ds4ld` by
  hand).
* Build tools and libraries:

| Distro | Packages |
| --- | --- |
| Debian, Ubuntu | `build-essential meson pkg-config libopus-dev libudev-dev libpipewire-0.3-dev dkms linux-headers-$(uname -r)` |
| Fedora | `gcc meson pkgconf-pkg-config opus-devel systemd-devel pipewire-devel dkms kernel-devel` |
| Arch | `base-devel meson opus systemd-libs libpipewire dkms linux-headers` |

`dkms` and the kernel headers are for the kernel module (see
[below](#the-kernel-module)).

## Install

```sh
git clone https://github.com/Mac898/DS4L
cd DS4L
./install.sh
```

This builds DS4L, installs it to `~/.local` (pass another prefix as an
argument if you like) and starts the `ds4l` user service. No root needed.

The first time a controller connects, the service installs its kernel
module through DKMS and asks for your password to do it. If a controller
was already connected then, turn it off and on again afterwards so it
picks up the new module. With Secure Boot on, the module has to be signed;
see [Secure Boot](#secure-boot).

## Check it works

Connect the controller, then:

```sh
systemctl --user status ds4l   # running
ds4l dkms status               # loaded=fork-loaded means the module is in
ds4l list                      # your controller, with audio=1 on Bluetooth
ds4l lightbar 255 0 0          # the lightbar turns red
```

For sound, pick **DualSense wireless controller (PS5)** as the output in
your audio settings and play something: it comes out of the controller's
speaker. Its microphone is the input of the same name.

If `ds4l` isn't found, `~/.local/bin` isn't in your `PATH`; `install.sh`
warns about that.

## Use

With one controller connected, no id is needed. With several, give the id
from `ds4l list`, or its first 4 or more characters.

```sh
ds4l lightbar 255 64 0               # orange
ds4l playerleds p2
ds4l trigger r weapon 2 7 8          # right trigger: gun-trigger effect
ds4l trigger r feedback 3 6          # resistance from 30% of travel
ds4l trigger r off
ds4l rumble 200 80
ds4l input                           # sticks, buttons, battery, sensors
ds4l status                          # connection and streaming counters
ds4l -h                              # everything else
```

Settings last until the controller disconnects or a game changes them.

## Remove

```sh
./uninstall.sh          # same prefix as install.sh
```

This stops the service and deletes the installed files. The kernel module
needs root, so it prints the commands that remove it instead of running
them.

## Troubleshooting

* Logs: `journalctl --user -u ds4l -f`, or `systemctl --user stop ds4l`
  and run `ds4ld -v` in a terminal.
* `ds4l dkms status` shows which `hid-playstation` module is loaded and
  which one modprobe would pick.
* Stock module still loaded after the DKMS install: a connected controller
  blocks the switch. Disconnect the controllers (or reboot), then
  `sudo modprobe -r hid_playstation && sudo modprobe hid_playstation`.
* Module installed but never loads: check Secure Boot (`mokutil
  --sb-state`) and `sudo dkms status`; the build log is in
  `/var/lib/dkms/hid-playstation-ds5/`.
* `permission denied` on `/dev/hidraw*`: the session has no seat access to
  the device (remote login, no logind). Install `udev/70-ds4l.rules`; the
  comments in it explain how.
* Haptics stutter, or the controller drops off Bluetooth while playing
  sound: see [Bluetooth adapters](#bluetooth-adapters).

## How it works

```
 apps (games, browsers, pw-record, ...)
   |                     |                  |
 evdev / js           PipeWire           ds4l CLI
   |                  sink / source          |
   |                     |             control socket
 ------------------------+------------------+---------  userspace
                         |                  |
                       ds4ld  (per controller: hidraw I/O, input parsing,
                         |     triggers, LEDs, Opus, haptics)
                         |
 hid-playstation (DS4L) -+--- hidraw  ----------------  kernel
   |  gamepad input, audio-mode reports dropped
   |
 DualSense  (Bluetooth or USB, any number)
```

`ds4ld` runs as your user. It finds controllers through hidraw, talks to
each one directly, and publishes a PipeWire sink and source per Bluetooth
controller. The sink is S16_LE, 4 channels at 48 kHz, like the wired sound
card: FL/FR go to the speaker (or a headset plugged into the controller),
RL/RR to the haptic actuators. Audio only goes over the air while
something plays; after half a second of silence the daemon stops sending.
Wired controllers keep their real USB sound card.

Measurements and protocol findings are in [doc/notes.md](doc/notes.md).

### The kernel module

In audio mode the controller mixes microphone data into its input
reports, and the stock `hid-playstation` driver reads those as gamepad
state: random stick movement, and the mouse flying around under Steam
Input. The fork in `driver/` drops those reports and is otherwise the
same driver. DKMS installs it into `updates/dkms`, so it overrides the
stock module and is rebuilt on kernel updates. Details are in
[driver/README.md](driver/README.md).

The daemon refuses audio mode while the stock module is loaded. Unless
started with `--no-auto-dkms`, it installs the fork with
`tools/ds4l-dkms-ensure.sh` through pkexec or sudo, from the installed
`share/ds4l` copy (or the source tree, for a build that isn't installed).

That helper and the driver sources run as root once you authorize them.
With the default `~/.local` install they are writable by your user, so
anything running as you could change them before you approve the prompt.
If that matters, install to a root-owned prefix (`sudo meson install` to
`/usr/local`), or install the module yourself with
`sudo driver/install-dkms.sh` and add `--no-auto-dkms` to the service's
`ExecStart`.

### Secure Boot

With Secure Boot on, the kernel only loads signed modules. Most distros'
DKMS signs with a Machine Owner Key once one is enrolled (`mokutil
--import`, then confirm at the next boot). `driver/install-dkms.sh` warns
if the installed module is unsigned.

### Bluetooth adapters

Audio quality depends on the adapter. An Intel 8260 stalls for ~11 ms
every 2.56 s (scan windows), felt as a haptic skip; `hciconfig hci0
noscan` avoids it, at the cost of discoverability. An Intel AX200 had no
periodic stalls. Apps that keep a Bluetooth discovery loop running, KDE
Connect for one, can kill the controller's connection. More in
[doc/notes.md](doc/notes.md); `tools/bt_gap_probe.py` measures your own
adapter.

If haptics stutter, `ds4l status` shows `late`/`maxgap`; raising the
keepalive (`ds4l keepalive 2`, or `ds4ld -k 2`) sends state more often.

### Several controllers and apps

Every controller's audio device has the same name, as wired ones do. An
app that wants a particular controller names its playback stream after
the controller's Bluetooth address (`media.name`, e.g.
"HD haptics (00-11-22-33-44-55)"; SDL apps set
`SDL_HINT_AUDIO_DEVICE_STREAM_NAME`), and the daemon moves that stream to
the controller's own device, Bluetooth or USB. Other streams are left
alone.

### Control protocol

`ds4l` talks to the daemon over `$XDG_RUNTIME_DIR/ds4l.sock`, which only
your user can open, one command per line. Replies are `ok ...` or `err ...`; see `src/ctl.c`.

* Daemon: `ping`, `help`, `list`, `dkms status|install`
* Device, as `<id|-> <verb> ...` (`-` is the first controller):
  * `status`, `input`, `audio on|off`, `keepalive 0..3`
  * `lightbar R G B`, `playerleds mask|p1..p5`, `micled on|off`
  * `rumble STRONG WEAK`, `power RUMBLE TRIGGER` (0-7 each)
  * `trigger l|r off|feedback ...|weapon ...|vibration ...|raw ...`
  * `volume SPK HP`, `micvol V`, `route hp|spk|both`, `mute MIC SPK HP`

### Source layout

* `src/device.c`: per-controller core (frame pacing, codecs, echo tracking)
* `src/manager.c`: hotplug, device set, audio gate, commands
* `src/pw_nodes.c`: PipeWire nodes
* `src/pw_route.c`: moves streams tagged with a controller's address to it
* `src/ds5_protocol.c`, `src/ds5_audio.c`: wire format and codecs
* `src/ctl.c`, `src/ctl_client.c`: control socket
* `cli/ds4l.c`: the CLI
* `driver/`: the kernel module
* `tests/unit.c`: tests that need no controller

### Building by hand

```sh
meson setup build
meson compile -C build
meson test -C build
./build/ds4ld -v          # foreground, logs to stderr
```

`libudev` and `libpipewire-0.3` are optional (`-Dudev=disabled`,
`-Dpipewire=disabled`): without libudev the daemon rescans every 2 s,
without PipeWire there are no audio devices and only the control side
works. `libopus` is required. The `ds4l` CLI links none of them.

## License

Userspace (everything except `driver/`): **LGPL-3.0-or-later**,
© 2026 Macauley Lim, [LICENSE](LICENSE), [COPYING.GPL-3](COPYING.GPL-3).

`driver/` derives from the in-kernel `hid-playstation` driver and is
**GPL-2.0-or-later** ([driver/COPYING](driver/COPYING)).

DualSense is a trademark of Sony Interactive Entertainment; this project
is unaffiliated.
