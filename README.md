# AMOLED Badge

Firmware for a round 466×466 AMOLED badge built on the Waveshare
**ESP32-S3-Touch-AMOLED-1.75** (the 32 MB flash / 8 MB PSRAM variant).

It is a thing you wear. It runs about a day on a charge, wakes to a clock, and
does a handful of things well rather than many things badly.

| | |
|---|---|
| **Trackpad / Air Mouse** | Bluetooth HID. Use it as a touchpad, or point with it — the gyro drives the cursor |
| **Clock** | Digital LCD face, plus timer, stopwatch and alarm |
| **Recorder** | 16 kHz IMA-ADPCM to flash, about 52 minutes. Plug in and it appears as a read-only USB drive full of `.WAV` files |
| **Games** | Brick breaker (5 levels), pinball, marble maze, bubble wrap |
| **Fidgets** | A pool of water and a small solar system, both driven by the accelerometer |
| **Calc · Keys** | A calculator, and a set of shortcut keys over the same HID link |

There is no cloud anything. WiFi is used for exactly two things — setting the
clock, and only when the clock is stale — and you type the network on the badge
itself.

## Flash it without building

If you only want the firmware on a board, there is a browser flasher — no
toolchain, nothing to install:

**<https://curisama.github.io/amoled-badge/flash/>**

It needs Chrome or Edge (WebSerial is not in Safari or Firefox). For those,
grab the binaries from a release and use `esptool`; the page lists the
offsets.

## Build

You need [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
and nothing else; every other dependency is fetched for you on first build.

```sh
git clone https://github.com/curisama/amoled-badge
cd amoled-badge
cp main/secrets.example.h main/secrets.h   # optional, may stay empty
idf.py build
idf.py -p /dev/ttyACM0 flash               # COMx on Windows
```

`tools/setup.sh` does the same thing and works out which OS you are on.
The first build downloads roughly 20 MB of components and takes a while; after
that it is quick.

> **The flash size matters.** The partition table assumes 32 MB. The same board
> is sold with less, and the recording partition will not fit.

## Simulator

The whole UI builds and runs on a PC — no board needed. This is how most of it
was written and how it is tested.

```sh
python3 sim/build.py
./sim/badge_sim                 # writes screenshots to sim/shots/
PORT=8792 python3 sim/server.py # then open http://localhost:8792
```

The simulator has a fake IMU and a fake battery, so tilt games and the
power-related screens work there too.

## Recordings

Recordings never leave the badge on their own. Open the recorder, press
**export**, and the badge re-enumerates as a read-only USB drive; the files
show up as `REC0001.WAV` and play in anything. Press **done** and it goes back
to being a serial port.

> Recording other people is regulated differently depending on where you are.
> In some places consent from one party to the conversation is enough; in
> others every participant has to agree. Find out which one applies to you
> before you use this.

## Layout

```
main/              firmware
  apps/            one file per app
  lcdface.c        the clock face, drawn as shapes
  port.h           everything hardware-specific sits behind this
  port_esp.c         the board's half
sim/               the same UI on a PC
  port_sim.c         the simulator's half of port.h
tools/             asset baking, regression checks, setup
docs/SETUP.md      toolchain setup per OS
```

`port.h` is the seam. If a file outside `port_esp.c` talks to hardware
directly, that is a bug.

## Checks

```sh
tools/regress.sh
```

It runs in a second and catches the things that have actually broken here
before — not a test suite so much as a list of scars.

## License

MIT, see `LICENSE`. Third-party components are listed in `NOTICE`; none of them
are redistributed in this repository.
