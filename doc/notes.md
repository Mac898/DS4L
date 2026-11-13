# DS4L notes

Measurements and findings the code depends on.

## Audio frames

The controller consumes one audio frame every 10.67 ms (93.75/s):

* 64 haptic bytes: 32 samples at 3 kHz, int8.
* One Opus frame of 480 samples, which plays at 45 kHz. It covers 512
  samples of 48 kHz source audio.
* Built from 16-sample blocks: 1 haptic sample + 15 speaker samples.
* Haptics and speaker travel in one frame. A haptic segment sent without
  its frame is discarded.

## Bluetooth report types

* `0x31`: state only (rumble, LEDs, triggers, volume). Carries only the
  fields flagged valid.
* `0x36`: audio frame, ~398 bytes, with the state block embedded. Used for
  frames with haptics and speaker, ~94/s.
* `0x39`: two speaker-only frames, no state, ~47/s. The frame counter
  advances by 2.
* `0x32`: ~142 bytes, haptics with no speaker audio. Sent instead of `0x36`
  when the speaker is silent, because the 398-byte report itself disturbs
  delivery (2-3x more late state reports in testing). Also used once to turn
  the mic on or off.
* A config report (motor power 0, low-pass on, mute byte) goes out ahead of
  each stream.

## State keepalive

The pad drives its actuators for only ~17 ms after the last state report.
`0x32`, `0x36` and `0x39` without state are silent (tested). A bare `0x31`
per frame works, as does the state embedded in a `0x36`. All-zero state is
enough.

So each haptic frame is followed by `keepalive` extra bare `0x31` reports,
spaced evenly across the frame. Default 1, so state arrives every ~5.3 ms.
Range 0-3: `ds4ld -k N`, or `ds4l keepalive N` per controller. 0 is enough on
an adapter with no stalls; `ds4l status` shows `late`/`maxgap` to judge it.

## Silence

The PipeWire sink runs whenever its driver does, so silence would be
encoded and sent forever. After 0.5 s of digital silence the stream stops.
Audio mode follows "Bluetooth and DS4L module loaded", not PipeWire stream
state.

## Input side

In audio mode the controller sends input at ~575-700 reports/s. Each
state report carries a host timestamp that the controller echoes in its
input reports. The time between echo changes shows how evenly state is
arriving; that is the `late`/`maxgap` figure in `ds4l status`.

## Bug: choppy speaker playback

Speaker audio came out in bursts (84% duty). A lone speaker segment was
sent while waiting for its pair, but not consumed, so the same segment went
out again a period later. That made ~195 reports/s against the ~94-100 the
pad sustains, and it dropped about one frame in six.

Rule: whatever goes on the wire must be consumed in the same step, or the
packet counter stalls and frames repeat.

## Wired USB audio

From `/proc/asound/card*/stream0`:

```
Playback: S16_LE, 4 ch, 48000 Hz, map FL FR RL RR  (FL/FR speaker/jack, RL/RR haptics)
Capture:  S16_LE, 2 ch, 48000 Hz, map FL FR
Description: "DualSense wireless controller (PS5)"
```

The Bluetooth nodes in `src/pw_nodes.c` copy this.

## Kernel module detection

* Loaded: `/sys/module/hid_playstation/parameters/ds5_drop_all_input`
  exists, so the DS4L module is loaded. If `/sys/module/hid_playstation`
  exists without it, the stock module is loaded.
* Installed: `modinfo -n hid_playstation` under `/updates/dkms/` means the
  DKMS build outranks the in-tree one.

## Games

* SDL has no adaptive-trigger API. Games send raw reports with
  `SDL_SendGamepadEffect`, and SDL's PS5 driver writes output reports
  itself. Those writes coexist with the daemon's.
* Proton games find the DualSense audio endpoint by name and format.
  GE-Proton has `PROTON_SONY_WINDOWS_DEVICE_NAMES=1` for this; our nodes
  use the same name and format as the USB card.
* Proton matches controller endpoints on hotplug by container ID. One
  stable identity per pad across evdev, SDL, PipeWire and the control
  socket isn't done.

## Bluetooth adapters

* **Intel 8260:** delivery stalls ~11 ms, 2-3 times a second, repeating
  every 2.56 s (page/inquiry scan windows). Felt as a haptic skip every ~3 s.
  `hciconfig hci0 noscan` fixes it, at the cost of incoming connections and
  discoverability.
* **Intel AX200** (Wi-Fi off in BIOS): no periodic stalls, default scan
  settings. A 60 s tone + haptics run: stalls over 20 ms at 0.14/s (worst
  22 ms), no 2.56 s pattern, no dropped frames, ~90 frames/s, worst send
  lateness 114 us, input 694.6 reports/s.
* **Discovery loops:** an app that keeps classic inquiry running (KDE
  Connect's scanner) can kill the pad's HID session. bluetoothd logs
  `hidp_send_message() BT socket write error: EAGAIN`, the ACL stays up
  with no hidraw, and the pad has to be power-cycled. Turn scanning off in
  such apps before chasing random disconnects.
* **No antenna:** the link collapses ~15 s into streaming even though
  Bluetooth seems fine at short range. Check `hcitool rssi` and `hcitool lq`.

`tools/bt_gap_probe.py /dev/hidrawN [seconds]` timestamps input reports and
prints interval percentiles, gap events and a gap-spacing histogram that
shows periodic scan windows. It can run alongside the daemon.
