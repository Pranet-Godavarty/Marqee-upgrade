# Marquee firmware

Arduino sketch for the Raspberry Pi Pico 2 on the controller board. It scrolls the lines of `messages.txt` across the 8 LED rows and keeps the current under what the board's 1 oz copper can carry.

This firmware was written with AI. It compiles for the Pico 2, but it hasn't been tested on the real marquee yet.

## Uploading it

1. Install the [Arduino IDE](https://www.arduino.cc/en/software).
2. In **File → Preferences → Additional boards manager URLs**, add:
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
3. In **Boards Manager**, install **Raspberry Pi Pico/RP2040/RP2350** by Earle F. Philhower.
4. In **Library Manager**, install **Adafruit NeoPXL8**, **Adafruit GFX Library** and **RTClib**, and say yes to their dependencies.
5. Select **Tools → Board → Raspberry Pi Pico 2**.
6. Select **Tools → Flash Size → 4MB (Sketch: 3MB, FS: 1MB)**. The messages drive is stored in that 1 MB, so the sketch won't start without it.
7. Open `marquee/marquee.ino`. Hold the Pico's BOOTSEL button while you plug in USB the first time, then click **Upload**.

## Changing the messages

Plug the board into a computer and it shows up as a USB drive. Open `messages.txt`, put one message per line, save, then eject the drive. The new messages start scrolling straight away.

```
// Lines starting with // are ignored
FRC TEAM 75
#00FF00 GO TEAM
#FFFFFF {TIME}
```

- `#RRGGBB` at the start of a line sets that line's colour. Lines without it are red.
- `{TIME}` and `{DATE}` are replaced with the time and date from the DS3231 clock.

## Setting the clock

The clock sets itself to the upload time the first time it runs, or whenever the coin cell has died. To set it by hand, open the Serial Monitor at 115200 baud and send:

```
T2026-10-08 09:30:00
```

## Settings

These constants are at the top of `marquee.ino`:

| Setting | Default | What it does |
|---|---|---|
| `LEDS_PER_ROW` | 150 | Pixels in each row |
| `FLIP_ROW` | all `false` | Set a row to `true` if its strip is fed from the right end |
| `SCROLL_MS` | 30 | Milliseconds per one-pixel scroll step (lower is faster) |
| `BRIGHTNESS` | 255 | Overall brightness, 0–255 |
| `ROW_LIMIT_MA` | 3000 | Current cap per row in mA |
| `HALF_LIMIT_MA` | {4000, 2000} | Current cap for rows 1–4 and rows 5–8 in mA. Rows 5–8 stay at 2000 until more vias are added to half B's input on the PCB, then set it to 4000. |

## How the current cap works

The board uses 1 oz copper. Its row traces can carry about 3 A and each half's input traces about 4 A, so those are the limits.

Before each frame, the sketch counts the lit pixels in every row and estimates the current: about 20 mA per colour channel at full brightness, plus 1 mA per LED when it's off. If any row would go over `ROW_LIMIT_MA`, or either half over its `HALF_LIMIT_MA`, the whole frame is dimmed just enough to fit, so all rows stay the same brightness. Scrolling text only lights part of each row, so normally nothing gets dimmed.

## Status LED (GP22)

| Blink | Meaning |
|---|---|
| Slow (1 s on, 1 s off) | Running normally |
| Fast (10 times a second) | The LED driver didn't start |
| Medium (about 3 times a second) | No filesystem: check the Flash Size setting in step 6 |
