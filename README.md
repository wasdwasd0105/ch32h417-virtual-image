# nanoCH32H417 USB 3.0 image mounter

Firmware that turns a [nanoCH32H417](https://github.com/wuxx/nanoCH32H417) (WCH CH32H417,
dual-core RISC-V, USB 3.0 5 Gbps) into a **virtual optical drive and disk**. Put `.iso`, `.img`,
`.vhd` or `.vmdk` files on a microSD card, pick one with a button, and the computer at the other
end of the cable sees an ordinary USB 3.0 DVD drive or removable disk -- one it can browse,
install from or boot from, at up to **42 MB/s**, with nothing to install on the host.

The card itself stays visible as a second drive on the same cable, so the whole loop is: drop an
ISO on the card over USB, press the button, boot from it.

## What the host sees

One USB 3.0 mass-storage device with three logical units:

| LUN | Host device | Backed by |
|---|---|---|
| 0 | CD/DVD drive, 2048-byte blocks (SCSI MMC) | the mounted `.iso` -- no disc when none is mounted |
| 1 | removable disk, read/write | the microSD card itself |
| 2 | removable disk, 512-byte blocks | the mounted `.img` / `.vhd` / `.vmdk` -- no medium when none |

The LUN types are fixed on purpose: a host reads INQUIRY once, at enumeration, so a LUN that
turned from "disk" into "CD-ROM" when an image was mounted would stay a disk to the OS.

Disk-image formats, all served straight from the card:

| File | How it is served | `rw` |
|---|---|---|
| `.iso` | CD/DVD on LUN 0 | -- |
| `.img` | raw | yes |
| `.vhd`, fixed | raw, the 512-byte footer hidden | yes |
| `.vhd`, dynamic | through the BAT: 2 MiB blocks, unallocated ones read as zeros | no |
| `.vmdk`, monolithicFlat | the descriptor's `-flat.vmdk` companion, raw | yes |
| `.vmdk`, monolithicSparse | grain directory -> grain tables -> 64 KiB grains | no |

Differencing VHDs, split (`-s001.vmdk`) and streamOptimized VMDKs are not handled.

## Highlights

* **SuperSpeed with a real USB 2.0 fallback.** A link supervisor picks the transport, survives
  hot-plug in both directions, hubs, and the host rebooting mid-session.
* **No filesystem in the data path.** The file's cluster chain is walked once into an extent
  table; every host read maps to a run of card sectors and streams with one SD multi-block
  command, so the virtual DVD is as fast as the raw card.
* **Swap images without unplugging.** The image LUN goes empty and comes back with a new medium;
  the card volume stays mounted on hosts that poll (a host that does not poll gets a 300 ms
  re-plug instead, so nobody has to know to eject anything).
* **A button and a screen.** A short press cycles `/imgs` and saves the choice to the card; the
  kit's 1.54" ST7789 shows card, image, link and live transfer rates.
* **The card is a normal USB drive** while all this runs -- writes go through a write-behind
  path that keeps small-block copies near 38 MB/s.
* Driven from a serial console, or from a USB keyboard plugged into the board's USB-FS port.

## Hardware

* **nanoCH32H417** (CH32H417QEU6). The USB 3.0 Type-C port goes to the host; the board is
  powered from it (or from the on-board WCH-LinkE).
* **microSD card**, FAT32 or exFAT, MBR or GPT -- including the GPT+exFAT layout macOS gives
  large cards. Files above 4 GB are fine on exFAT.
* *Optional* **1.54" 240x240 ST7789 panel** from the kit, on J8 (FPC-12P): DC=PD9, CS=PB12,
  SCK=PB13, SDA=PB15, RESET=PD8, backlight hard-wired.
* *Optional* **push button** between `C12` (PC12) and the neighbouring `GND` on the top-left
  header. The board's own MODE/IAP buttons belong to the WCH-LinkE chip, not to the MCU.
* *Optional* **USB keyboard** on the board's USB-FS Type-C port -- the firmware enumerates it as
  a host and it drives the console. Close the `VBUS 5V` header next to that connector, or the
  keyboard gets no power.
* **Console:** USART1, PA9/PA10, 115200 8N1. On this board it reaches the WCH-LinkE's USB-serial
  port through solder bridges SB4/SB3.

## Build

Needs the MounRiver RISC-V GCC 12 toolchain (`riscv-wch-elf-gcc`; the chip uses the `xw`
extension). Point `TOOLCHAIN` at its `bin` directory, at the top of the `Makefile` or on the
command line, then:

```bash
make CLOCK=400P
```

That produces `build/merge.bin` -- the boot core (V3F) at offset 0 and the application core (V5F)
at 0x10000, the layout the WCH linker scripts and flash tools expect.

| Variable | Default | Meaning |
|---|---|---|
| `CLOCK` | `350` | `350` = V5F 350 MHz / HCLK 175 / SDIO_CK 87.5 MHz; `400P` = 400 / 200 / 100 MHz. Use `400P`: it is the only setting where both SD modes below are clean on this board. |
| `VIO18_LEVEL` | `3` | boot level of the VIO18 I/O supply that feeds the SD pins (3 = 3.3 V). The driver moves it to 1.8 V itself for UHS-I; leave it at 3. |
| `BOOT_DELAY_MS` | `2500` | how long the boot core waits before starting the application core -- the window a debugger can attach in. |

## Flash

`build/merge.bin` is a plain image for flash offset 0. Write it with whatever WCH tooling you
already trust -- MounRiver Studio, WCH-LinkUtility, `wlink`, or OpenOCD with the `wch_riscv`
driver. The board carries a WCH-LinkE, so its own USB-C cable is enough to program it; keep the
USB 3 cable plugged in as well if you want the board powered while you work.

The scripts under `tools/` are what was used here (macOS + OpenOCD): `flash_once.sh` is
resumable, and `flash_soft.sh` reflashes hands-free by resetting the chip through the firmware's
own console. They are conveniences, not requirements.

## Prepare the card

Format FAT32 or exFAT and copy images into the root or into `/imgs`:

```
/imgs/ubuntu-24.04.iso
/imgs/rescue.img
/ISOMOUNT.TXT          (optional)
```

`/imgs` is the folder the button cycles through; the root is used as well (and as a fallback when
the folder is empty). `tools/make_test_images.sh` builds a test ISO and a raw image if you want
something to check against, and `tools/make_rw_image.sh` builds a writable one.

## Choosing an image

At boot the firmware mounts, in this order of preference:

1. the image named in `ISOMOUNT.TXT` in the card root,
2. the last image chosen on the console (kept until power-off),
3. the only image on the card, if there is exactly one,
4. nothing -- and the list is printed on the console.

`ISOMOUNT.TXT` is two short lines:

```
imgs/ubuntu-24.04.iso
rw
```

Line 1 is the file (an `imgs/` prefix picks the folder copy over a root file of the same name;
`none` means "boot with nothing mounted"). Line 2 is optional and only meaningful for disk
images: `rw` exposes the image read-write.

### The button

* **Short press** -- rescan the card, then mount the next image in `/imgs`, cycling
  `none -> 1 -> 2 -> ... -> none`. **The choice is saved to the card**, so the next boot comes up
  the same way. Files added or removed while the card was in a computer are picked up by the next
  press.
* **Long press (>= 1 s)** -- switch between the two SD modes (below).

Console `N` does the same as a short press, `U` the same as a long one.

The choice is saved by rewriting `ISOMOUNT.TXT` **in place** as a fixed 512-byte record: one
aligned sector write, no directory, FAT or bitmap change, so a host that has the card mounted
does not see its cached metadata go stale. The file is created or re-padded only at boot, when no
host can be looking.

### Swapping while the host is connected

A swap does not re-enumerate USB. The image LUN goes empty for a second and then presents the new
medium as a not-ready-to-ready change, with a unit attention and an MMC media-change event. The
firmware also **refuses PREVENT ALLOW MEDIUM REMOVAL**, the way a card reader does -- that refusal
is what keeps hosts polling the LUN, because a drive the OS could lock is a drive it never polls.

* Windows and Linux poll both kinds of LUN every 1-2 s and pick a swap up on their own.
* macOS polls disk images the same way, but its optical driver stops polling once a disc is in,
  so an ISO swap would otherwise sit behind the old volume until you ejected it.

For a host that has still not looked at the changed LUN after `IMG_SWAP_ACK_MS` (2.5 s), the
firmware falls back to the one signal every OS honours: a 300 ms USB re-plug, which re-enumerates
everything, the card volume included. Measured on macOS: new ISO mounted 4.3 s after the press,
card back 4.8 s. Set `IMG_SWAP_ACK_MS` to 0 to keep the card volume alive instead and leave the
old disc for the user to eject.

## The status screen

Nine rows of 12x24 text: card model and size, bus mode and clock, the mounted image with its
position in the cycle, the USB link (`USB 3.0` green, `USB 2.0` yellow, `USB waiting host`), live
read and write rates with bars, and a bottom line that shows the key hint until an SD error or
retry happens, then the counters. Rows are redrawn only when their text changes, from the idle
loop between USB commands, so transfers lose well under 1%.

**The panel and UHS-I cannot be on at the same time.** Its CS/SCK/DC pins sit in the chip's VIO18
domain, which UHS-I switches to 1.8 V, and the ST7789 needs 3.3 V logic. Hence two modes, toggled
by a long key press or console `U`:

| Mode | SD bus | SDIO_CK | Display | Measured (Samsung 512 GB, 256 MiB, uncached) |
|---|---|---|---|---|
| **1 LCD** (boot default) | 3.3 V High Speed | 50 MHz | live | W 20.1 / R 21.5 MiB/s, 0 errors |
| **2 UHS-I** | 1.8 V SDR50 | 100 MHz | frozen, with a note | W 38.0 / R 41.8 MiB/s, 0 errors |

Without the panel connected, mode 2 is simply the faster mode; `SD_TRY_UHS` in `app_config.h`
picks the boot default.

## Speed

Samsung 512 GB card, mode 2 (UHS-I SDR50), USB 3.0 SuperSpeed:

| Workload | Result |
|---|---|
| 4.3 GB ISO read through the virtual DVD, verified byte-exact | **42.4 MB/s** |
| writable 4 GiB FAT32 image (`rw`) | 35.8 MB/s write / 44.1 MB/s read |
| 2.2 GB `cp` onto the card LUN, md5 verified | 37.9 MB/s |
| 128 KiB uncached writes (Finder-style), with write-behind | 38.9 MiB/s |
| reads behind a USB 2.0 hub, in mode 1 | ~20 MB/s (the card's mode-1 ceiling, not the bus) |

The image is streamed from its own sectors, which is why a virtual disk is as fast as the card
underneath it. A *fragmented* image is the case to watch: if writes drop to 20-30 MB/s, the host
has scattered its clusters and every command becomes its own stream -- `tools/fat32_check.py`
will show it and `tools/fat32_quickformat.py` gives a clean image back.

## Console

USART1 at 115200, or a USB boot keyboard on the board's USB-FS port -- the firmware enumerates it
as a host and feeds the keys into the same dispatcher (`k` reports what it found; the `VBUS 5V`
header next to the connector has to be closed). Card-touching commands are queued and run between
USB transfers.

| Key | Action |
|---|---|
| `l` | list the images on the card |
| `1`..`9` | mount image N (always read-only) |
| `N` / `U` | next image in `/imgs` / toggle SD mode -- the two key presses |
| `e` / `E` | eject the mounted image (`E` forces it while the host holds the lock) |
| `s` / `p` | status / statistics: SD errors, sectors moved, write-behind counters |
| `u` | USB link: phase, live link state, recent control requests, counters |
| `i` / `t` / `n` | re-initialise the card / SD self-test / flip the SDIO sampling edge |
| `C` | CRC-32 of the mounted image through its format mapping |
| `P` / `L` | force a USB re-plug / re-insert the card LUN after ejecting it on the host |
| `X` | delete `/.Spotlight-V100` from the card (macOS mount repair, see below) |
| `D` / `V` | LCD colour test / halve the panel's SPI clock |
| `r` / `h` | reset the chip / help |

`Z j J q Q W R` are a manual lab for the link supervisor (pause it, force either controller on or
off, hard reset) -- useful when bringing up a new host, harmless otherwise.

## How it works

### Images are streamed, not read through a filesystem

FatFs (exFAT, long names, GPT) walks the file's cluster chain **once**, at mount, into a table of
contiguous SD sector runs (up to `IMG_MAX_EXTENTS` = 256). From then on every host READ/WRITE is
mapped block -> file sector -> card sector through that table and streamed by the SD driver with
one CMD18/CMD25 per run; no filesystem code runs in the data path. Container formats read their
allocation tables (VHD BAT, VMDK grain tables) on demand through a 32 KiB cache mapped over the
same extents, and an unallocated block is answered as zeros without touching the card. Each
16 KiB USB chunk is always filled completely, even when it spans extents -- a short bulk packet
in the middle of a data phase would end the host's transfer.

### CD/DVD emulation

Follows the Linux mass-storage gadget's `cdrom=1` behaviour: READ TOC (formats 0/1/2, including
the old SFF-8020i form macOS sends), READ HEADER, GET CONFIGURATION, GET EVENT STATUS
NOTIFICATION, READ DISC INFORMATION, READ DISC STRUCTURE, MODE SENSE page 2Ah, READ CD,
eject/load through START STOP UNIT, and REPORT LUNS. Rejected data-in commands end with a
zero-length packet and a failed CSW rather than a STALL, which USB 3 hosts handle far better.

### Write-behind

Finder and most copy tools write in ~128 KB pieces. Closing the card's multi-block write after
each command would cost a few milliseconds of programming time per command, so a WRITE that
continues exactly where the previous one stopped pushes straight into the open CMD25 instead. The
stream is flushed on anything else: a non-sequential write, any read, SYNCHRONIZE CACHE, a
console action, ~20 ms of idle, an eject, or the host going away. Every block is still
acknowledged by the card before its CSW goes out -- only the CMD12 and the final programming wait
are deferred, as on any caching drive.

### USB transport

The chip has two device controllers, USBSS for SuperSpeed and USBHS for USB 2.0, and they share
the connector *and* the single USB 2.0 PHY: only one can run at a time. One supervisor in the
main loop (`usb_link_poll()` in `v5f/usbss_device.c`) owns that decision; the interrupt handlers
only record what they saw.

| Phase | Running | Leaves when |
|---|---|---|
| `wait` | USBSS (the boot state) | SuperSpeed enumerates -> `SS-active`; or a host has been on D+/D- for 1.8 s with the link still in RxDetect -> `HS-active` |
| `SS-active` | USBSS | the link stays dead for 0.5 s -> unplugged -> reset -> `wait` |
| `HS-active` | USBHS | the bus is idle for 0.8 s and D+/D- float again -> reset -> `wait` |

Every disconnect hard-resets both USB blocks and starts over from the SuperSpeed attempt: on this
part the states that work reliably are the ones a fresh power-up reaches, so returning to the boot
state beats un-wedging a half-torn-down controller.

One rule on top of the phases: **once a host has enumerated the board at SuperSpeed on a cable,
the firmware never falls back to USB 2.0 for that host.** If the link drops while the host's
pull-downs are still there -- it is rebooting, or resetting its USB controller -- the board keeps
the SuperSpeed attempt alive (a fresh RxDetect every 2 s) until it comes back, exactly like a real
USB 3 drive. The USB 2.0 fallback is only for a host that never trained SuperSpeed since the cable
was plugged in.

## Configuration

`v5f/app_config.h`, all compile-time:

| Knob | Default | Meaning |
|---|---|---|
| `SD_TRY_UHS` | `0` | boot mode: 0 = 3.3 V + live display, 1 = UHS-I 1.8 V |
| `SD_CLKDIV_3V3_SAFE` | `2` | SDIO_CK after init in mode 1 (2 = 50 MHz at HCLK 200) |
| `IMG_SUBDIR` | `"imgs"` | folder the key cycles |
| `IMG_CONFIG_FILE` | `"ISOMOUNT.TXT"` | the saved-choice record in the card root |
| `IMG_MAX_EXTENTS` | `256` | contiguous runs a mounted file may consist of |
| `KEY_SWAP_GAP_MS` | `1000` | how long the image LUN stays empty across a swap |
| `IMG_SWAP_ACK_MS` | `2500` | a host that has ignored the change this long gets a USB re-plug (0 = never) |
| `USB_HS_FALLBACK_MS` | `1800` | no SuperSpeed progress this long after an attach = a USB 2.0 host |
| `KEY_ENABLE`, `LCD_ENABLE` | `1` | the button and the panel |
| `USB_VID` / `USB_PID` | `1A86:55D1` | USB identity, plus the SCSI INQUIRY strings below them |

## Limitations and gotchas

* **LCD or UHS-I, not both** -- the panel's control pins live in the SD pins' voltage domain.
  Mode 2 freezes the panel with a note instead of drawing garbage.
* **The firmware writes to the card only** to save the image choice, and only into the existing
  512-byte record. Everything else it does to the card is reads: a second writer next to the host
  would put the FAT at risk.
* **One writer at a time for a mounted `rw` image.** While it is mounted read-write, do not touch
  the file through the card drive -- the mounter's writes go straight into those sectors. Eject
  the image on the computer first.
* `rw` is honoured only from `ISOMOUNT.TXT`; console mounts are read-only on purpose.
* Dynamic VHD and sparse VMDK are read-only; differencing, split and streamOptimized containers
  are not read at all.
* The slot has **no card-detect pin and no switchable card power**, so a card that stops
  answering can only be recovered by pulling it out and putting it back -- the firmware cannot
  power-cycle it, and cannot tell an empty slot from a silent card.
* **Two USB devices on one cable is not possible here** (a card drive on SuperSpeed, a DVD on
  USB 2.0, so an image swap could never disturb the card): the two controllers share one USB 2.0
  PHY, and a device controller answers one address, so hub emulation is out too.
* macOS can refuse to mount a card whose Spotlight index got damaged. Console `X` deletes
  `/.Spotlight-V100` on the card (macOS rebuilds it) without needing any privileges on the Mac.

## Verified on

* **macOS**, USB 3.0 and USB 2.0: ISO and disk images mounted and verified byte-exact, live cable
  moves between a USB 3 port and a USB 2.0 hub, image swaps, writable images, a 2.2 GB copy onto
  the card.
* **A Windows laptop**: SuperSpeed enumeration, booting from the virtual DVD as far as the ISO's
  GRUB menu, and surviving a Ctrl+Alt+Del reboot of the host without dropping to USB 2.0.

## Layout

```
Makefile                 both cores, MounRiver GCC 12
v3f/                     boot core: clocks, VIO18, starts the application core
v5f/main.c               application core: init, then the MSC task
v5f/image_store.[ch]     image scan, ISOMOUNT.TXT, container formats, extent table, sector mapping
v5f/msc_core.[ch]        USB mass storage: BOT + SCSI/MMC, three LUNs, streaming read/write
v5f/sd_sdio.[ch]         SDIO driver: identification, High Speed / UHS-I, streaming engine
v5f/usbss_device.c       SuperSpeed device + the link supervisor (usb_link_poll)
v5f/usbhs_msc.c          the USB 2.0 device side
v5f/usbfs_host.c         USB full-speed host on the second port: a keyboard drives the console
v5f/console.[ch]         the console and the PC12 key
v5f/display.[ch]         the status page
v5f/lcd_st7789.[ch]      ST7789 driver, polled SPI2, no framebuffer
v5f/leds.c               D1 blue = activity, D2 green = image mounted
v5f/app_config.h         every knob in one place
v5f/fatfs/               FatFs R0.15 + the disk glue
lib/                     WCH CH32H417 peripheral library, startup code and linker scripts
tools/                   build, flash, image-building and verification helpers
docs/development-log.md  how all of this was found out
```

## Tools

| Script | What it does |
|---|---|
| `tools/make_test_images.sh` | build a test ISO and a raw image, copy them to a card |
| `tools/make_rw_image.sh` | build a writable FAT32/exFAT image and select it with `rw` |
| `tools/mkvdisk.py` | wrap a raw image into fixed/dynamic VHD and flat/sparse VMDK |
| `tools/verify_image.py` | check a mounted image against its checksums, measure read speed |
| `tools/fat32_check.py` | mini fsck for a FAT32 image *file*, read through the card volume |
| `tools/fat32_quickformat.py` | rewrite an image's FATs, root and FSInfo, then verify them |
| `tools/fs_stress.py` | fsync-heavy mixed-size write/verify workload |
| `tools/console.py`, `monitor.py` | serial console helpers |
| `tools/mkfont.py` | render a monospace TrueType face into `font12x24.h` |

None of them need `sudo`: they work on the image file through the card's own volume.

## Documentation

[`docs/development-log.md`](docs/development-log.md) is the working log this README grew out of --
the measurements, the host-behaviour findings (what macOS's SCSI drivers actually poll, why a
STALL alone is not enough on USB 3) and the dead ends, in the order they were hit.

## License

Copyright (C) 2026 wasdwasd0105.

This program is free software: you can redistribute it and/or modify it under the terms of the
**GNU General Public License version 3**, as published by the Free Software Foundation, or (at
your option) any later version. It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY -- without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. The full text is in [LICENSE](LICENSE).

That covers this project's own sources: `v3f/`, `v5f/` (except `v5f/fatfs/`), `tools/`, the
`Makefile` and the documentation. Vendored third-party components keep their own terms:

* `lib/` -- CH32H417 peripheral library, startup code and linker scripts from the WCH EVT,
  Copyright (c) 2025 Nanjing Qinheng Microelectronics. Their notice limits use to microcontrollers
  they manufacture, so these files are redistributed under WCH's terms, not under the GPL.
* `v5f/fatfs/` -- FatFs R0.15, Copyright (C) 2022 ChaN, under its own one-clause BSD-style licence
  (see the header of `ff.h`); GPL-compatible.
* The CD/DVD emulation follows the behaviour of the Linux kernel's mass-storage gadget; no code
  was taken from it.
