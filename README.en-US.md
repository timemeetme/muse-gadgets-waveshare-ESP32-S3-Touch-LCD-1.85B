# Waveshare ESP32-S3-Touch-LCD-1.85B — Firmware Flashing & Usage Guide

[简体中文](README.md) | **English**

This repository adds community board support for the **Waveshare
ESP32-S3-Touch-LCD-1.85B** round-screen dev board to Meta's open source
[Muse Gadgets](https://gadgets.muse.ai/) ESP32 Device SDK
([facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)).
Once flashed, this 1.85-inch round touch wristwatch board becomes a Muse
gadget: full on-screen avatar UI, push-to-talk voice conversation, touch
settings, image display, battery reporting, and OTA updates.

- Hardware: [Waveshare product page](https://www.waveshare.com/esp32-s3-touch-lcd-1-85b.htm) ·
  [Waveshare Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85B) (work in progress)
- Upstream protocol & feature docs: [esp32/README.md](esp32/README.md), [esp32/AGENTS.md](esp32/AGENTS.md)
- Board entry: [esp32/devices/README.md](esp32/devices/README.md)

---

## Contents

1. [What this is / Differences from the 1.85C](#1-what-this-is--differences-from-the-185c)
2. [Preparation](#2-preparation)
3. [Install ESP-IDF v6.0.1 (the only supported version)](#3-install-esp-idf-v601-the-only-supported-version)
4. [Get the source and configure your SDK token](#4-get-the-source-and-configure-your-sdk-token)
5. [Build the firmware](#5-build-the-firmware)
6. [Back up the factory firmware (strongly recommended)](#6-back-up-the-factory-firmware-strongly-recommended)
7. [Flash](#7-flash)
8. [Watch the logs (serial monitor)](#8-watch-the-logs-serial-monitor)
9. [First boot and pairing](#9-first-boot-and-pairing)
10. [Daily use](#10-daily-use)
11. [Restore factory firmware / factory reset](#11-restore-factory-firmware--factory-reset)
12. [FAQ](#12-faq)

---

## 1. What this is / Differences from the 185C

The 1.85B is a sibling of the upstream-supported
[1.85C](https://www.waveshare.com/esp32-s3-touch-lcd-1-85c.htm). Screen, touch
and audio are identical; the differences are confined to IO expansion and
battery sensing:

| Part | 1.85C | 1.85B |
|---|---|---|
| Round 360×360 ST77916 display (QSPI) | Same | Same |
| CST816S touch | Same | Same |
| Audio: ES8311 (speaker) + ES7210 (dual mics) | Same | Same |
| Display reset | TCA9554 expander EXIO2 | **Direct GPIO3** |
| Touch reset | TCA9554 EXIO1 | **Direct GPIO1** |
| PA (amp) enable | GPIO15 | **GPIO9** |
| Battery sensing | GPIO8 ADC + resistor divider | **BQ27220 fuel gauge (I2C, addr 0x55)** |
| TCA9554 IO expander | Present | **None** |
| Flash / PSRAM | 16 MB / 8 MB octal | Same |
| USB | Native chip USB Serial/JTAG (`303a:1001`) | Same |

All firmware changes for this port live in:

- Board driver: [`esp32/components/muse/boards/board_waveshare_s3_185b.c`](esp32/components/muse/boards/board_waveshare_s3_185b.c)
- Vendor panel init sequence: [`esp32/components/muse/boards/waveshare_s3_185b/st77916_init_185b.h`](esp32/components/muse/boards/waveshare_s3_185b/st77916_init_185b.h)
- Build config overlay: [`esp32/devices/sdkconfig.muse-waveshare-s3-185b`](esp32/devices/sdkconfig.muse-waveshare-s3-185b)
- Wiring (Kconfig / CMake / component rules / `board.sh` / `ports.py` / `avatar.py`) and doc tables.

Battery readings come from standard BQ27220 commands: voltage `0x08`,
instantaneous current `0x0C` (positive = charging), relative state of charge
`0x2C`. The gauge is powered by the battery, so **without a battery attached
the reading returns `-1`**. This is an expected graceful degradation and does
not affect anything else.

## 2. Preparation

**Hardware**

- One Waveshare ESP32-S3-Touch-LCD-1.85B
- A **USB data cable** (many USB cables are charge-only; flashing needs data)
- A computer (Windows 10/11 primarily; macOS / Linux commands are noted per section)

**Account & app**

1. Register / sign in at [gadgets.muse.ai](https://gadgets.muse.ai/), go to
   **Account → SDK tokens** (<https://gadgets.muse.ai/settings/sdk-tokens>),
   and create a token (looks like `mgst_...`). **Every gadget pairing needs a
   token.** Also read the [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms).
2. Install the **Muse app** (iOS / Android) on your phone. In
   **Settings → Devices**, first enable **Developer mode** — only then will
   community devices show up.

**Computer software**

- Python 3.9+ (`python` runs from your shell)
- Git
- ESP-IDF **v6.0.1** (installed in the next section; other versions are not supported)

## 3. Install ESP-IDF v6.0.1 (the only supported version)

This firmware only builds under ESP-IDF **v6.0.1**. If you hit missing-header
errors such as `gcm.h` or `gpio_ll.h`, it is almost always the wrong IDF
version.

### Windows

Option 1 (command line, recommended):

```powershell
# 1) Clone IDF (shallow + submodules; takes a while)
git clone --depth 1 --recursive -b v6.0.1 https://github.com/espressif/esp-idf.git D:\esp\esp-idf-v6.0.1

# 2) Install the xtensa-esp32s3 toolchain (one time; downloads compiler, cmake, ninja, ...)
cd D:\esp\esp-idf-v6.0.1
install.bat esp32s3
```

Option 2: use the [ESP-IDF Windows installer](https://dl.espressif.com/dl/esp-idf/)
and pick **v6.0.1** in the version list.

Then, in **every new terminal**, activate the environment before building or
flashing:

```powershell
D:\esp\esp-idf-v6.0.1\export.bat
```

> `export.bat` is a batch file; calling it directly from PowerShell will not
> persist the environment. Run it in **cmd**, or use
> `& D:\esp\esp-idf-v6.0.1\export.ps1` in PowerShell if available, or fold the
> activation and the build into a single cmd command as shown in section 5.

### macOS / Linux

```sh
git clone --depth 1 --recursive -b v6.0.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.1
cd ~/esp/esp-idf-v6.0.1 && ./install.sh esp32s3
. ~/esp/esp-idf-v6.0.1/export.sh
```

## 4. Get the source and configure your SDK token

```powershell
git clone https://github.com/timemeetme/muse-gadgets-waveshare-ESP32-S3-Touch-LCD-1.85B.git
cd muse-gadgets-waveshare-ESP32-S3-Touch-LCD-1.85B\esp32
```

Every app build needs your own SDK token (`mgst_...`). The token lives in the
**sdkconfig inside the build directory** (that file is Git-ignored and will
never be committed; also **do not** put it into any overlay).

Workflow: build once to generate `build-muse-waveshare-s3-185b/sdkconfig`,
then fill in the token and rebuild:

```powershell
# (complete the first build in section 5, then come back here)
notepad build-muse-waveshare-s3-185b\sdkconfig
```

Find (or append) this line and fill in your token:

```ini
CONFIG_GADGET_SDK_TOKEN="mgst_your_token"
```

Save and rerun the build command from section 5. Building without a token
works (with a warning), but the device will not be able to pair once Muse
enforces token validation.

## 5. Build the firmware

Run from the repository's `esp32` directory.

### Option 1: board.sh one-shot (needs bash; macOS / Linux / Git Bash)

```sh
tools/muse/board.sh build 185b
```

`board.sh` sets the target chip and the three config layers
(`sdkconfig.defaults` → `devices/sdkconfig.muse` → `devices/sdkconfig.muse-waveshare-s3-185b`)
and builds into the dedicated `build-muse-waveshare-s3-185b/` directory.
It looks for ESP-IDF in the order `~/.espressif/esp-idf-v6.0.1` → `IDF_PATH` →
`IDF_EXPORT`.

### Option 2: idf.py directly (recommended on Windows without bash)

Fold the IDF activation and the build into a single cmd command (run from
PowerShell):

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig -DSDKCONFIG_DEFAULTS=""sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b"" build"
```

(On macOS / Linux the equivalent is:

```sh
idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b" build
```

)

The first build downloads dependencies into `managed_components/` via the
component manager, so it needs network access and takes a while. On success
the binary is at `build-muse-waveshare-s3-185b/muse-gadget.bin`.

**Verification**: the `check_sizes` line in the build log shows no partition
overflow; you can also confirm that the generated
`build-muse-waveshare-s3-185b/sdkconfig` contains
`CONFIG_MUSE_BOARD_WAVESHARE_S3_185B=y`, `CONFIG_IDF_TARGET="esp32s3"`, and a
16 MB flash size.

> Note: once `sdkconfig` exists, later changes to overlays are not picked up
> automatically. After editing an overlay or `sdkconfig.defaults`, delete
> `build-muse-waveshare-s3-185b/sdkconfig` (or the whole build directory) and
> rebuild.

## 6. Back up the factory firmware (strongly recommended)

Flashing overwrites the stock firmware. Before your first flash, dump the
entire 16 MB flash and store the backup **outside the Git repository**:

Connect the board over USB and identify the COM port in Device Manager (this
board uses the chip's native USB and shows up as "USB Serial Device (COM x)";
`COM7` below is an example — use your own port):

```powershell
python -m esptool --chip esp32s3 -p COM7 -b 460800 read-flash 0 0x1000000 waveshare-s3-185b-backup.bin
```

The backup is about 16 MB. Keep it somewhere safe.

## 7. Flash

### Option 1: idf.py (simplest)

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-waveshare-s3-185b/sdkconfig -DSDKCONFIG_DEFAULTS=""sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185b"" -p COM7 flash"
```

### Option 2: esptool + flash_args (equivalent)

```powershell
cd build-muse-waveshare-s3-185b
python -m esptool --chip esp32s3 -p COM7 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
cd ..
```

### Option 3: board.sh (bash)

```sh
tools/muse/board.sh flash 185b        # auto-detects the serial port by USB ID
tools/muse/board.sh flash 185b COM7   # or specify it explicitly
```

**Notes**

- Flashing writes the bootloader, partition table, otadata, phy_init and the
  app; **NVS is untouched** — reflashing for an upgrade keeps your pairing
  info and Wi-Fi credentials.
- `idf.py -p COM7 erase-flash` wipes the whole flash (pairing included); use
  it only when you want a clean slate.
- Can't enter the bootloader (stuck at `Connecting...`): **hold BOOT, tap
  RESET once, release BOOT**, then flash. After flashing, press RESET once or
  power-cycle to run the firmware.
- The firmware ships with the repository's development signing keys; Secure
  Boot is disabled and no eFuses are burned. Do not enable Secure Boot / flash
  encryption on a board you still want to reflash freely.

## 8. Watch the logs (serial monitor)

```powershell
cmd /c "call D:\esp\esp-idf-v6.0.1\export.bat && idf.py -B build-muse-waveshare-s3-185b -p COM7 monitor"
```

Exit with `Ctrl-]`. A healthy boot shows:

```
link.main: Muse Gadget starting
```

followed by a line `muse: board: Waveshare ESP32-S3-Touch-LCD-1.85B`.
For non-interactive capture (auto-reset, collect a few seconds):

```sh
python tools/muse/monitor.py COM7 10
```

## 9. First boot and pairing

1. After flashing, the firmware runs immediately: the screen lights up with
   the avatar UI. While unpaired, the screen shows the device name
   **`MuseGadget-XXXXXX`** (the tail of the device MAC) so you can spot it in
   the app.
2. Open the phone **Muse app → Settings → Devices** (Developer mode on) and
   scan for the `MuseGadget-XXXXXX` device.
3. The app provisions Wi-Fi over BLE (pick your network, enter the password)
   and pairs. When the app asks you to **confirm on the device**,
   **short-press the BOOT button** to accept.
4. Once paired, the device joins Wi-Fi, establishes an encrypted session with
   Muse, and the screen shows the green/connected state; the device name
   disappears and the microphone icon appears.

**The two BOOT button functions**:

- **Short press**: confirm a pending pairing; if setup is not finished, reopen the settings window
- **Long press (5 s)**: reset settings (unpair and forget Wi-Fi)

**Status colors** (shown in the on-screen status area / edge):

| Color | Meaning |
|---|---|
| Orange, breathing | BLE advertising, waiting for provisioning |
| Blue, breathing | Pairing waiting for on-device confirmation |
| Blue | Wi-Fi / session connecting |
| Green | Connected |
| Yellow, blinking | Session switching or connection lost |
| Purple | Not paired |
| Red, blinking | Error |

This device uses community pairing v5; the phone app must support v5 and show
community devices.

## 10. Daily use

- **Voice chat (push to talk)**: **press and hold the avatar** on the screen
  for about half a second to start recording; release to send. Holding the
  **BOOT** button also works as push-to-talk. Muse transcribes and answers;
  replies can play from the speaker (TTS) and show on screen.
- **Touch settings**: open the on-screen settings UI to adjust brightness and
  more (options evolve with firmware versions; the screen is the source of
  truth).
- **Image display**: Muse can send images to the device, shown directly on
  the round display.
- **Battery**: Muse reads voltage / percentage / charging state from the
  BQ27220 gauge (`device.health`). Plugged into USB, the state shows
  "charging".
- **OTA updates**: OTA is enabled by default on this board (Muse can push
  firmware updates remotely). Local re-flashing (section 7) is always
  available and does not affect pairing.
- **Standby**: on battery, with the screen off and no voice activity, the
  device sleeps in a low-power state; BOOT or touch wakes it. On USB power it
  runs at full speed.
- **Change the avatar**: with the serial port attached and the device paired
  and online, run `python tools/muse/avatar.py` from the `esp32` directory and
  follow the prompts to have Muse draw a custom avatar and flash it in.

## 11. Restore factory firmware / factory reset

- **Restore the stock firmware**: write the section 6 backup back verbatim:

  ```powershell
  python -m esptool --chip esp32s3 -p COM7 -b 460800 write-flash 0 waveshare-s3-185b-backup.bin
  ```

- **Just unpair / clear Wi-Fi**: long-press BOOT for 5 s, or
  `idf.py -p COM7 erase-flash` (the latter wipes everything — pairing, Wi-Fi
  and NVS data — and you will need to pair again).

## 12. FAQ

**Q: `python -m esptool` can't connect to the board?**
Use a USB data cable (not charge-only); confirm the port in Device Manager;
if needed hold BOOT, tap RESET to enter download mode. This board uses the
native USB Serial/JTAG (USB ID `303a:1001`) with no driver needed; if it is
enumerated as something else, try another cable or USB port.

**Q: Build fails with missing `gcm.h` / `gpio_ll.h`?**
Your ESP-IDF is not v6.0.1. Install the correct version per section 3.

**Q: Edited an overlay but nothing changed?**
The generated `build-muse-waveshare-s3-185b/sdkconfig` takes precedence.
Delete it (or the whole build directory) and rebuild.

**Q: Device stuck purple / unpaired?**
Check that the SDK token was written into
`build-muse-waveshare-s3-185b/sdkconfig`, then rebuild and reflash; make sure
the app has Developer mode on and supports community pairing v5.

**Q: Battery shows `-1` or no reading?**
Battery readings come from the BQ27220 gauge, which is powered by the
battery. With no battery installed (USB only) this is expected. If readings
are still missing with a battery attached, check the battery flex cable.

**Q: Display garbled / abnormal?**
Make sure you flashed the 185B firmware, not the 185C (the reset pins differ;
the wrong firmware breaks the display). Verify
`muse: board: Waveshare ESP32-S3-Touch-LCD-1.85B` in the serial monitor.

**Q: Building for other boards too?**
Boards share `esp32/managed_components/` and `dependencies.lock`. If the
component manager complains when switching between display and non-display
boards, delete those two and rebuild.

---

## Acknowledgements & license

- Upstream SDK: Meta Platforms, Inc. and contributors, Apache License 2.0
  (see [LICENSE](LICENSE)).
- This port builds on the upstream 1.85C board implementation and the
  publicly documented ST77916 panel init sequence from
  [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32).
- Flashing is at your own risk: possible warranty void, bricking, etc.
