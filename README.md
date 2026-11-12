# RE6 Splitscreen Gyro

Gyro aiming for **both players** in Resident Evil 6 (PC) splitscreen.

Steam Input's usual gyro setup ("gyro to mouse") only works for one player,
because the game accepts a single mouse. This mod reads each controller's gyro
through Steam Input and turns that player's own camera directly. Both players
get real gyro aiming at the same time, and their right sticks keep working.

- Works while walking and while aiming, for each player independently.
- Recalibrates gyro drift automatically: set the controller down for a second.
- Settings are re-read while the game runs, so you can tune them live.
- Tested on Linux (Proton) with two Nintendo Switch Pro Controllers.
  Should work on Windows and with other gyro controllers Steam Input supports
  (DualShock 4, DualSense, Steam Deck, Steam Controller), but these are untested.

## Requirements

- Resident Evil 6 on **Steam** (the current Steam build; the mod checks the game
  code it patches and does nothing if the version doesn't match).
- Steam Input enabled for your controllers.
- Windows 10/11, or Linux/Steam Deck with Proton. (Windows 7/8 need the
  Universal C Runtime update, KB2999226.)

## Install

1. Copy `xinput1_3.dll` and `re6gyro.ini` into the game folder, next to `BH6.exe`.
   (Steam: right-click Resident Evil 6 → Manage → Browse local files.)
2. **Linux / Steam Deck (Proton) only:** set the game's launch options
   (right-click → Properties → General → Launch Options) to:

   ```
   WINEDLLOVERRIDES="xinput1_3=n,b" %command%
   ```

   Windows doesn't need this step.
3. **Turn off Steam's own gyro** for RE6 on every controller: in the
   controller layout, set the gyro to *none / disabled*. If Steam's
   gyro-to-mouse stays on, it fights the mod.
4. Start the game and play splitscreen. Player 1 and player 2 each aim with
   their own controller's gyro.

## Settings (`re6gyro.ini`)

Changes apply within a second, even with the game running.

| Setting | Default | What it does |
|---|---|---|
| `enabled` | `1` | `0` turns gyro off. |
| `sensitivity` | `2.0` | Camera degrees per degree you turn the controller. `1.0` is 1:1. |
| `sensitivity_pitch` | `1.0` | Up/down sensitivity relative to left/right. |
| `invert_yaw` | `0` | `1` reverses left/right. |
| `invert_pitch` | `0` | `1` reverses up/down. |
| `player_space` | `1` | `1` = turn by yawing *or* rolling the controller, however you hold it (recommended). `0` = only the controller's own vertical axis. |
| `disable_button` | `none` | Hold this button to pause gyro, e.g. to re-centre your hands: `a b x y lb rb ls rs back start up down left right`. Xbox button names; on a Switch Pro controller `a`/`b` and `x`/`y` are swapped, depending on your Steam settings. |

`[advanced]`

| Setting | Default | What it does |
|---|---|---|
| `counts_per_dps` | `16.31` | Gyro units per degree/second, measured on a Switch Pro Controller. Only change this if a different controller turns too fast or slow at `sensitivity=1.0`. |
| `deadzone_dps` | `0.0` | Ignore rotation slower than this (deg/s). |
| `still_dps` | `2.5` | Below this, the controller counts as "still" for drift calibration. |
| `log_level` | `1` | `0` = minimal log. |
| `record` | `0` | `1` = write raw gyro samples to `re6gyro_record.csv` (calibration). |
| `debug` | `0` | `1` = log each player's camera state changes to `re6gyro_log.txt`. Turn on when reporting a bug. |

Settings apply to both players.

## Troubleshooting

The mod writes `re6gyro_log.txt` in the game folder each time the game starts.

- **No log file appears:** the DLL isn't being loaded. On Linux, check the
  launch option above. Make sure the file is named exactly `xinput1_3.dll`
  and sits next to `BH6.exe`.
- **`unsupported BH6.exe version`:** your game build differs from the one the
  mod was made for. The camera isn't touched, so the game plays normally,
  without gyro.
- **`controllers: 0`:** Steam Input isn't handling your controllers. Enable
  Steam Input for RE6 (controller settings in the game's Steam properties).
- **Camera drifts slowly:** put the controller down for a second to
  recalibrate. If it keeps drifting, raise `still_dps` a little.
- **The camera fights you / jumps:** Steam's gyro-to-mouse is still on. Turn it
  off in the controller layout.
- **One player's gyro moves the wrong camera:** please report it with your
  log file.

## Uninstall

Delete `xinput1_3.dll` (and optionally `re6gyro.ini` and `re6gyro_log.txt`)
from the game folder, and remove the launch option on Linux.

## How it works

- `xinput1_3.dll` is loaded by the game in place of the system XInput DLL. All
  controller input is passed through to the real system DLL unchanged.
- It connects to Steam Input through the game's own Steam connection and reads
  each controller's motion data at about 500 Hz. Steam reports each
  controller's player slot, so each player's gyro goes to the right camera.
- It hooks the game's camera function that turns right-stick input into camera
  angles (`uCameraQFPS`, at `0x615c40`). After the game applies the stick, the
  mod adds that player's gyro rotation, and applies the game's own pitch limits
  and wrap-around. That function covers every camera mode, aiming included.
- It finds which controller drives each camera from the game's own
  player → controller → port number data.
- Gyro motion during menus and cutscenes is discarded, so the camera doesn't
  snap when you get control back.

## Building from source

Requires a MinGW-w64 i686 cross compiler (`i686-w64-mingw32-gcc`).

```
make            # build/xinput1_3.dll
make release    # build/release.zip
make install GAMEDIR="/path/to/Resident Evil 6"
```
