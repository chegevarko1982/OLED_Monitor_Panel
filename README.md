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
This hardware has the multiplexer at address `0x71`.
The firmware selects the panel driver based on address parity (`_addrI2C & 0x01`):

| Address | Driver |
|---|---|
| even (`0x70`, `0x72`, ...) | SH1106 |
| odd (`0x71`, `0x73`, ...) | SSD1306 |

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

Two screens gate their value and show dashes outside it, because the sim keeps sending a
number where the instrument would show nothing:

| Screen | Digits | Valid range | Outside the range |
|---|---|---|---|
| Radio Altimeter | 4, leading zeros | 0..2500 ft | `----` |
| VOR DME | 3, leading zeros | 1..999 | `---` |

A non-numeric value shows dashes as well - MobiFlight sends `"-0"` for VOR DME when no
station is tuned.

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

For details on the partial-update rendering optimization and display refresh performance, see `documents/PLAN_partial_update.md`.

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

### Digit animation (optional)

`RADIO ALT` and `VOR DME` can roll their digits like an odometer instead of
snapping. Both are driven by the sim rather than by a knob, so the value walks
in small steps and the roll reads as a counter.

It is off unless asked for. In the board settings dialog, the custom device has
an **Additional Config** text field:

| String | Effect |
|---|---|
| *(empty)* | animation off - the default, and what an already-configured board keeps |
| `ANIM=RA+DME` | radio altimeter and DME, 4 frames |
| `ANIM=RA` | radio altimeter only |
| `ANIM=ALT+HDG+CRS` | any combination, joined with `+` |
| `ANIM=RA\|FRAMES=6` | 6 frames instead of 4 |
| `ANIM=ALL` | every screen |
| `ANIM=OFF` | explicitly off |

Screen names: `RA` radio altimeter, `DME` VOR DME, `MACH` the Mach/speed screen,
`SPD` FCU speed, `HDG` FCU heading, `ALT` FCU altitude, `CRS` course, `ALL` all of
them. V/S is the one screen with no name, because it has no fixed digit cells -
its sign and digits move between fonts and positions with the V/S and FPA modes,
so it takes a full repaint either way. `ALL` simply skips it.

`FRAMES` is 2..8 at 25 ms per frame, so the default 4 gives a 100 ms roll. Keys
and screen names are case insensitive and their order does not matter. A string
that cannot be parsed switches animation off and reports

```
Custom Device: bad Config - use ANIM=RA+DME|FRAMES=4
```

back to the connector, rather than half-applying itself in silence.

The list separator is `+`, and the characters `,` `;` `/` `.` `:` must not appear
in this field at all. The first three are the CmdMessenger field, command and
escape characters and the last two terminate fields and devices in the board's
stored config, so any of them truncates the entry on its way to the EEPROM -
which leaves the config field unterminated and makes the whole custom device
disappear from the board on the next restart. Nothing in the settings dialog
warns about this.

A transition never animates when it would be meaningless: into or out of the
dashes (radio altimeter above 2500 ft, DME with no station), during Light Test,
or when a step moves more than two digits at once - `300` to `299` clicks over.
That last one is also the frame budget: one cell costs 7.1 ms of the 25 ms frame.

### Aircraft profile

The package ships **no** `.mcc` aircraft profile, by choice. The one inherited
from the original project was bound to `serial="Gagagu FCU-EFIS/ SN-397-88A"` -
the original author's board - and still referenced message ids 0/1/3/4/7 from
the seven-screen layout this firmware dropped, so it could not have attached to
anything. A `.mcc` always records the board it was exported from, so a shipped
profile has to be re-pointed at the installing user's board anyway; assigning
the outputs from scratch against the message list above is no more work.

If you ever do want to ship one, drop it in
`OLED_Monitor_Panel/Community/profiles/msfs2020/` and rebuild - the whole
`Community` folder is staged into the ZIP, nothing else needs changing.

## Credits

Original FCU/EFIS firmware and connector configuration by Gagagu; community-device integration by
elral. This variant only changes the screen count and layout.
