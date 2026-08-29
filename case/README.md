# Snap-fit enclosure

Two printed parts, no screws and no inserts: a back tub that holds the board, and a front bezel that plugs into it on four cantilever fingers.

```
openscad -o back.stl  -D 'part="back"'  mono-note-mini.scad
openscad -o front.stl -D 'part="front"' mono-note-mini.scad
openscad -o plate.stl -D 'part="plate"' mono-note-mini.scad
```

**Open `plate.stl` in the slicer, not the two parts separately.** Both parts are
modelled at the same origin, so loading `back.stl` and `front.stl` together puts
the bezel *inside* the tub where you cannot see it. `plate.stl` lays them out
side by side.

## Shape

The profile is a **squircle** - the superellipse `|x/a|^n + |y/b|^n = 1` at
n = 4, rather than a rectangle with radiused corners. Curvature is continuous
the whole way round, so there is no point where a straight edge meets an arc.
(n = 2 would be an ellipse, n -> infinity a rectangle; Apple's icon is n ~ 5 and
so not strictly a squircle.) Change `sq_n` to taste.

It is not only cosmetic. The wall is no longer straight, so the snap fingers
had to be re-derived from the curve: at their old positions the barb lost all
engagement at its inner end and the case would have popped open.

Open the file in [OpenSCAD](https://openscad.org) and set `part` to `"assembly"` to see it together, or `"plate"` to lay both out for printing.

## The board already comes in a case

Worth knowing before printing anything: the ESP32-S3-ePaper-1.54 **ships inside a finished white enclosure**, closed with two screws on the back. This model is a *replacement* for it, not the only way to house the board — if the stock case is fine, you do not need this at all.

What it changes is how it opens. Two screws become six snap hooks and a thumb notch, so you can get at the SD card without a driver.

## What is known, and what is not

**Known**, from Waveshare's outline drawing, and used directly:

| | mm |
|---|---|
| Outside | 39.80 × 53.00 × 16.90 |
| Corner profile | squircle, n = 4 |
| Display window | 27.80 square |
| Window position | centred horizontally, 14.30 up from the bottom edge |

The device is **portrait** — taller than it is wide. The glass itself is 200 px at 0.138 mm pitch = 27.6 mm, so the 27.80 window already carries 0.1 mm of margin per side. The aperture exposes all of it deliberately: the UI puts its back button in the bottom rows of the panel, so a bezel lapping even 2 mm over the display would sit on a control.

**Not known.** Waveshare dimensions the cased product and never the bare PCB, so everything in the `ASSUMED` block still waits on calipers:

| Variable | What to measure |
|---|---|
| `pcb_w`, `pcb_h` | PCB outline, corner to corner |
| `pcb_bottom` | from the case's bottom edge to the bottom of the PCB |
| `front_stack` | tallest thing on the display side, from the PCB face |
| `back_stack` | tallest thing on the back — TF socket, battery header or speaker |
| `btn_side`, `btn_boot_z`, `btn_pwr_z` | which edge the side buttons are on, and how far up |
| `usb_x` | USB-C centre along the bottom edge |
| `tf_z` | TF slot height up the right edge |
| `mic_x/y`, `spk_x/y` | microphone and speaker positions |

The outside will be the right size and shape as printed. The inside will not hold the board correctly until those are measured.

## Printing — Bambu A1, 0.4 mm nozzle

Start from **0.20 mm Standard @BBL A1**, then change four things:

| Setting | Value | Why |
|---|---|---|
| Wall line width | **0.40 mm** | makes the 1.6 mm finger exactly 4 walls and the 2.0 mm shell exactly 5 |
| Wall loops | **5** | so the shell is solid perimeters with no infill inside it |
| Top / bottom shells | **5** | 1.0 mm of solid either side of the 1.8 mm faces |
| Supports | **off** | nothing needs them, see orientation below |

The line width is the one that matters. At the default 0.42 mm a 1.6 mm finger works out at 3.8 walls, so the slicer puts a sliver of infill down the middle of it — and a cantilever with infill in its core snaps at the root the first time you flex it. At 0.40 it is four solid walls.

Infill 20 % gyroid is plenty; both parts are nearly all perimeter. Leave elephant-foot compensation at Bambu's default 0.15 mm — the bezel plugs in on a 0.25 mm clearance, and a squashed first layer eats straight into that.

### Orientation

- **Back tub:** floor down, open side up. As exported. Port cutouts are all on vertical walls and bridge fine.
- **Front bezel:** face down, fingers pointing up. As exported. The visible face comes off the plate smooth.

### Filament

**PETG for the bezel.** Printed upright, the fingers have their layer lines running across the beam, so bending one works the layer bonds rather than the plastic itself — that is where printed snaps fail, always at the root. PETG's layer adhesion is much better than PLA's and it takes more deflection before it yields. There is a gusset at each finger root to spread that load, but the material still helps.

PLA is fine for the tub, and fine for the bezel too if you are gentle the first few times. **Avoid carbon or glass filled filament entirely** — stiff and brittle is the exact wrong combination for a snap fit.

Both parts fit the A1's 256 mm bed with room to spare; print them together.

## How the snap works

Six cantilever hooks on the bezel catch grooves in the tub wall: two per long side, one per short side. `hook_len` is what keeps them alive — a 6.5 mm beam deflecting 0.9 mm stays inside PLA's elastic range, where a 3 mm beam doing the same job would stress-whiten and snap off after a few openings.

To take it apart, push a thumbnail into the notch on the bottom edge and lift. If it opens too easily, raise `hook_catch` by 0.1 mm at a time; if it will not close, raise `hook_clear` instead.

![assembly](assembly.png)

Back tub and front bezel:

![back](back.png) ![front](front.png)

## Verified geometry

Both parts render and export cleanly, and the meshes were checked directly:

| | back tub | front bezel |
|---|---|---|
| Triangles | 7204 | 1220 |
| Connected shells | 1 | 1 |
| Non-manifold edges | 0 / 10806 | 0 / 1830 |
| Footprint | 39.80 × 53.00 | 39.80 × 53.00 |
| Height | 15.10 | 1.80 + fingers |

15.10 + 1.80 = **16.90 mm assembled**, matching Waveshare's depth exactly, and neither part exceeds the footprint — the bezel plugs *into* the tub, so nothing protrudes.

## Still not verified

**Nothing has been printed.** The geometry is sound and the outside is the right size, but the internals rest on the assumed board dimensions above. Treat the first print as a draft-quality fit test.
