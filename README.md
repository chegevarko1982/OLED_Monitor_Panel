# OLED Monitor Panel

An 8-display OLED panel for MobiFlight, built as a community device.

Based on the work of [Gagagu](https://github.com/gagagu/Mobiflight-A320-Efis-Fcu-Display-with-ESP32)
and [elral](https://github.com/elral/MobiFlight-FCU_EFIS_OLEDs), extended from seven displays to
eight and re-purposed for a different screen layout.

## Hardware

- Arduino Mega 2560
- 8 × 1.3" OLED, 128×64, SH1106 or SSD1306
- TCA9548A I2C multiplexer

All displays share I2C address `0x3C` and are selected through the multiplexer.
Set the multiplexer address in MobiFlight to `0x70` or `0x71`:

| Address | Driver |
|---|---|
| even (`0x70`) | SH1106 |
| odd (`0x71`) | SSD1306 |

## Screen layout

| TCA channel | Screen | MobiFlight message |
|---|---|---|
| 0 | FCU SPD | 8, 9, 18 |
| 1 | MACH | 2, 9, 18 |
| 2 | CRS | 20 |
| 3 | FCU HDG | 10, 11 |
| 4 | Radio Altimeter | 6 |
| 5 | FCU ALT | 13, 14 |
| 6 | VOR DME | 5 |
| 7 | FCU V/S | 15, 16, 17 |

Messages 9 (speed managed) and 18 (speed/mach mode) affect both the FCU SPD and MACH screens.
Message 19 drives the lamp test on all eight screens; message 12 switches V/S between V/S and FPA mode.

## Building

```
pio run -e oled_monitor_panel_mega
```

The core MobiFlight firmware source is fetched automatically into `src/` at the tag set by
`custom_core_firmware_version` in `OLED_Monitor_Panel/oled_monitor_panel_platformio.ini`.

> **Note:** if your project path contains spaces, the git commands in `get_CoreFiles.py` must have
> their paths quoted, otherwise the core source silently fails to update and the build compiles
> against whatever version is already in `src/`. This is already fixed in this repository.

A release ZIP is written to `_dist/` by `copy_fw_files.py`.

### Version numbers must line up

`get_version.py` names the firmware `<env name>_<VERSION>.hex`, taking `VERSION` from the
environment and falling back to `0.0.1`. MobiFlight looks for the file named by
`FirmwareBaseName` + `LatestFirmwareVersion` from `board.json`.

So a plain local build produces `oled_monitor_panel_mega_0_0_1.hex` while the board files still
declare `LatestFirmwareVersion: 0.9.2` — MobiFlight will not find the firmware. Either build with
an explicit version:

```
VERSION=1.0.0 pio run -e oled_monitor_panel_mega
```

and set `LatestFirmwareVersion` to `1.0.0` in both `board.json` files, or leave the version at
`0.0.1` in the board files while testing locally. In a GitHub release build `VERSION` comes from
the release tag automatically.

## Installation

Extract the ZIP from `_dist/` into the `Community` folder of your MobiFlight installation, connect
the board and start MobiFlight. Flash the firmware, then add one custom device and set the I2C
address of the multiplexer.

## Credits

Original FCU/EFIS firmware and connector configuration by Gagagu; community-device integration by
elral. This variant only changes the screen count and layout.
