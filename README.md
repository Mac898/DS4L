# DS4L

DualSense for Linux.

One daemon for every DualSense and DualSense Edge, over Bluetooth or USB.
It adds what the stock Linux stack lacks: the speaker, HD haptics, the
microphone and adaptive triggers. Gamepad input stays with the kernel.

## Features

* Gamepad: buttons, sticks, triggers, touchpad, sensors, battery and LEDs,
  from the kernel `hid-playstation` driver as usual.
* Speaker and haptics (Bluetooth): a PipeWire sink named "DualSense wireless
  controller (PS5)", S16_LE 4 ch at 48 kHz. FL/FR go to the speaker, RL/RR
  to the actuators.
* Microphone (Bluetooth): a matching 2-channel PipeWire source.
* USB audio: the controller's own USB sound card, used as is.
* Adaptive triggers: feedback, weapon, vibration and raw modes, per trigger
  and controller.
* Lightbar, player LEDs, rumble, volume, routing and mute, from the `ds4l` CLI.

## Build

```sh
meson setup build
meson compile -C build
```

Needs C11 + pthreads and `libopus`. Optional: `libudev` (hotplug; without
it a 2 s rescan), `libpipewire-0.3` (the audio devices; without it only the
control plane). The DKMS module additionally needs `dkms`, `gcc`, `make`
and kernel headers, only when the daemon has to build it.

## Run

```sh
./build/ds4ld                        # foreground; logs to stderr
./build/ds4l list                    # connected controllers
./build/ds4l dkms status             # which kernel module is loaded
./build/ds4l dkms install            # build+install+reload it (also automatic)
```

With a controller connected (omit the id when only one is present):

```sh
ds4l lightbar 255 64 0               # orange
ds4l playerleds p2
ds4l trigger r weapon 2 7 8          # right trigger: gun-trigger effect
ds4l trigger r off
ds4l rumble 200 80
ds4l input                           # sticks/buttons/battery/sensors
```

To install for your user (default prefix `~/.local`) and enable the
systemd user service:

```sh
./install.sh
```

The DKMS module needs root, so the daemon builds and loads it on first run
and asks for authorization then (`--no-auto-dkms` to opt out). The daemon's
own hidraw access comes from systemd-logind's udev ACLs for the logged-in
user; no udev rule is needed.

## Audio

On Bluetooth pads, pick **DualSense wireless controller (PS5)** in your
audio settings. Each pad gets one playback and one capture device, the same
as a wired controller shows. Audio mode opens when a stream starts and
closes when the last one stops. USB pads use the real USB sound card and
need nothing (`ds4l audio on` is refused there).

## The kernel module

In audio mode the controller interleaves audio into its input reports, and
the stock `hid-playstation` reads them as gamepad state: random axes, and
the mouse flying around under Steam Input. The fork in `driver/` drops those
reports and is otherwise the same driver. It installs through DKMS into
`updates/dkms`, so it overrides the stock module and survives kernel updates.

The daemon refuses audio mode while the stock module is loaded. Unless
started with `--no-auto-dkms`, it installs the fork with
`tools/ds4l-dkms-ensure.sh` (pkexec or sudo). If a controller is bound at
that point, the new module takes effect on the next reconnect.

## Control protocol

One command per line over `$XDG_RUNTIME_DIR/ds4l.sock` (see `src/ctl.c`).
Replies are `ok ...` or `err ...`.

* Daemon: `ping`, `list`, `dkms status|install`
* Device, as `<id|-> <verb> ...`:
  * `status`, `input`, `audio on|off`, `keepalive N`
  * `lightbar R G B`, `playerleds mask|p1..p5`, `micled on|off`
  * `rumble S W`, `power R T`
  * `trigger l|r off|feedback ...|weapon ...|vibration ...|raw ...`
  * `volume S H`, `micvol V`, `route hp|spk|both`, `mute m s h`

## Bluetooth adapters

Audio quality depends on the adapter. An Intel 8260 stalls for ~11 ms every
2.56 s (scan windows), felt as a haptic skip; `hciconfig hci0 noscan`
avoids it. An Intel AX200 had no periodic stalls. Apps that keep a Bluetooth
discovery loop running, KDE Connect for one, can kill the controller's HID
session. Details and a probe for your own adapter are in
[doc/notes.md](doc/notes.md) and `tools/bt_gap_probe.py`.

## Layout

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

* `src/device.c`: per-controller core (frame pacing, codecs, echo tracking)
* `src/manager.c`: hotplug, device set, audio gate
* `src/pw_nodes.c`: PipeWire nodes
* `src/ds5_protocol.c`, `src/ds5_audio.c`: wire format and codecs
* `src/ctl.c`: socket protocol

## License

Userspace (everything except `driver/`): **LGPL-3.0-or-later**,
© 2026 Macauley Lim — [LICENSE](LICENSE), [COPYING.GPL-3](COPYING.GPL-3).

`driver/` derives from the in-kernel `hid-playstation` driver and is
**GPL-2.0-or-later** ([driver/README.md](driver/README.md)).

DualSense is a trademark of Sony Interactive Entertainment; this project is
unaffiliated.
