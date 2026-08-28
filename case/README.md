# Snap-fit enclosure

Two printed parts, no screws and no inserts: a back tub that holds the board, and a front bezel that clips over it on six cantilever hooks.

```
openscad -o back.stl  -D 'part="back"'  mono-note-mini.scad
openscad -o front.stl -D 'part="front"' mono-note-mini.scad
```

Open the file in [OpenSCAD](https://openscad.org) and set `part` to `"assembly"` to see it together, or `"plate"` to lay both out for printing.

## Before you print this

**Every board dimension in the file is an assumption.** Waveshare publishes the outline only as a drawing, so the numbers in the `MEASURED INPUTS` block are inferred from the 1.54" module and a typical dev-board layout. They are almost certainly wrong in detail.

Printing it as-is will produce a case that is the right *shape* and the wrong *size*. Measure these nine things with calipers and correct the block at the top — nothing below it needs to change:

| Variable | What to measure |
|---|---|
| `board_w`, `board_h` | PCB outline, corner to corner |
| `front_stack` | tallest thing on the display side, from the PCB face |
| `back_stack` | tallest thing on the back — usually the TF socket or battery header |
| `disp_off_x`, `disp_off_y` | centre of the 27.6 mm active area, relative to the PCB centre |
| `usb_x` | centre of the USB-C connector along the bottom edge |
| `tf_y` | centre of the TF slot along the right edge |
| `btn_boot_x`, `btn_pwr_x` | centre of each button along the bottom edge |
| `spk_x`, `spk_y`, `mic_x`, `mic_y` | speaker and microphone, on the back face |

The display's active area is the one number that is not a guess: 200 px at 0.138 mm pitch is 27.6 mm, and the bezel aperture is cut at 28.2 mm so it clears the glass without overlapping any of it. That matters because the UI puts a control bar in the bottom rows — a bezel that covers even 2 mm of the panel would sit on top of the back button.

## Printing

- **0.2 mm layers, 3 perimeters, 20% infill.** No supports needed: the hooks print standing up off the bezel face and the port cutouts are all on vertical walls.
- **PETG or PLA.** PETG flexes further before yielding, which suits snap hooks; PLA works but be gentler on the first assembly. Avoid brittle filled filaments — carbon-fill will crack the hooks.
- Print the bezel **face down** so the visible surface is the smooth plate side.

## How the snap works

Six cantilever hooks on the bezel catch grooves in the tub wall: two per long side, one per short side. `hook_len` is what keeps them alive — a 6.5 mm beam deflecting 0.9 mm stays inside PLA's elastic range, where a 3 mm beam doing the same job would stress-whiten and snap off after a few openings.

To take it apart, push a thumbnail into the notch on the bottom edge and lift. If it opens too easily, raise `hook_catch` by 0.1 mm at a time; if it will not close, raise `hook_clear` instead.

## Not yet verified

Nothing here has been printed, and no STL has been exported — OpenSCAD is not installed on the machine this was written on, so the geometry has not been rendered or checked for self-intersection. Treat the first print as a fit test, in draft quality, before committing to a good one.
