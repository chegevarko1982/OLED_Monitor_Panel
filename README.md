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
environment and falling back to `0.0.1`. MobiFlight looks for exactly the file named by
`FirmwareBaseName` + `LatestFirmwareVersion` in `board.json`, so those two have to agree or the
package installs and then fails to flash.

`board.json` declares `LatestFirmwareVersion: 1.0.0`, so a package meant for MobiFlight must be
built with that version:

```
VERSION=1.0.0 pio run -e oled_monitor_panel_mega
```

A plain `pio run` produces `oled_monitor_panel_mega_0_0_1.hex`, which is fine for flashing over
USB during development — MobiFlight is not involved — but is not a package you can install. In a
GitHub release build `VERSION` comes from the release tag automatically.

Only the Mega is shipped. There is no Pico build environment, so no Pico board file is included
either: listing a board whose firmware is not in the package gets it as far as the flash button
and no further.

## Installation

Extract the ZIP from `_dist/` into the `Community` folder of your MobiFlight installation, connect
the board and start MobiFlight. Flash the firmware, then add one custom device and set the I2C
address of the multiplexer.

### Digit animation (optional)

Any screen can roll its digits like an odometer instead of snapping to the new
value. It is off unless asked for: the custom device has an **Additional
Config** text field in the board settings dialog, and an empty field means the
panel behaves exactly as it did before animation existed.

#### Screen names

One name per screen, matching the layout table above.

| Name | Screen | TCA channel | Shows | Animates |
|---|---|---|---|---|
| `SPD` | FCU SPD | 0 | selected speed, 3 digits | when not managed |
| `MACH` | MACH | 1 | Mach `X.YZ`, or speed in SPD mode | when not managed |
| `CRS` | CRS | 2 | HSI course, 3 digits | always |
| `HDG` | FCU HDG | 3 | selected heading, 3 digits | when not managed |
| `RA` | Radio Altimeter | 4 | radio altitude, 4 digits | inside 0..2500 ft |
| `ALT` | FCU ALT | 5 | selected altitude, 5 digits | always |
| `DME` | VOR DME | 6 | DME distance, 3 digits | with a station tuned |
| `VS` | FCU V/S | 7 | vertical speed, sign + 4 digits | in V/S mode only |
| `ALL` | all eight | | | |
| `OFF` | none | | | explicitly off |

Two of these deserve a word. `MACH` is one screen, not two: it carries the Mach
value and the selected speed, and message 18 switches between them - the name
covers the screen either way. `VS` animates in V/S mode but not in FPA mode,
which draws a decimal point and a different digit count and so is repainted
whole; message 12 switches between them.

#### Writing the string

```
ANIM=<names joined with +>|FRAMES=<2..8>
```

| String | Effect |
|---|---|
| *(empty)* | animation off - the default, and what an already-configured board keeps |
| `ANIM=ALL` | every screen, 4 frames |
| `ANIM=RA+DME` | radio altimeter and DME only |
| `ANIM=ALT+HDG+CRS` | any combination, joined with `+` |
| `ANIM=RA\|FRAMES=6` | 6 frames instead of 4 |
| `ANIM=OFF` | explicitly off |

`FRAMES` is 2..8 and sets how briskly a wheel turns: 8, the default, brings a
one-digit change round in about 96 ms, 4 in 60 ms, 2 in 36 ms. It is a speed,
not a step count - a wheel re-aimed while it is still turning simply has
further to travel. Names and keys are case insensitive and their order does not
matter, so `frames=2|anim=all` is the same string. A value that cannot be
parsed switches animation off entirely and reports

```
Custom Device: bad Config - use ANIM=RA+DME|FRAMES=4
```

back to the connector, rather than half-applying itself in silence.

#### Characters this field must not contain

The list separator is `+` because `,` `;` `/` `.` and `:` are all unusable
here. The first three are the CmdMessenger field, command and escape
characters; the last two terminate fields and devices in the board's stored
config. Any of them truncates the entry on its way to the EEPROM, which leaves
the config field unterminated and makes **the whole custom device disappear
from the board**. Nothing in the settings dialog warns about this.

#### When a change does not animate

A transition is snapped rather than rolled only when rolling would be
meaningless:

- into or out of the dashes - radio altimeter above 2500 ft, DME with no
  station tuned, any managed screen showing `---`;
- during Light Test, and on the first value after it.

There is no longer a cap on how many digits may turn. Any number of them, on
any number of screens, roll together - including a full five-digit carry on the
altitude screen, `09900` to `10000`.

#### What that costs, and why it is nearly free

The firmware draws exactly one digit per 12 ms frame period, whatever else is
moving. So the load does not depend on how many wheels are turning:

| | 1 digit turning | 28 digits turning |
|---|---|---|
| longest stretch with serial blocked | 7.1 ms | 7.1 ms |
| bus busy | 59 % | 59 % |
| RAM | 120 B | 120 B |

7.1 ms against 22 ms of serial buffering, and the RAM is allocated for all
five cells of all eight screens whether they turn or not.

What *does* grow with the number of wheels is how long they take to settle,
because they share those 12 ms slots between them. That is paid for by turning
each wheel in coarser steps the more of them are moving, so the settle time
stays roughly flat instead of growing in proportion:

| digits turning at once | settle time | if the step never coarsened |
|---|---|---|
| 1 | 96 ms | 96 ms |
| 2 | 132 ms | 192 ms |
| 3 | 156 ms | 288 ms |
| 5 - a full ALT carry | 180 ms | 480 ms |
| 12 - four screens at once | 348 ms | 1152 ms |

A wheel never turns in fewer than two steps, so the coarsest case is still a
roll and not a click. The earlier firmware took the other route - it refused to
animate past three digits and snapped them instead - and with every screen
enabled a fourth moving digit somewhere on the panel is the common case, not a
rare one. That was the "some digits roll, some click" behaviour.

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
