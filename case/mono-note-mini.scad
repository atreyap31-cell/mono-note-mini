// Mono Note Mini - snap-fit enclosure, no screws
// =============================================================================
// Two parts: a back tub that holds the board, and a front bezel that clips over
// it on six cantilever hooks. Printed in PLA or PETG at 0.2 mm, no supports.
//
//   openscad -o back.stl  -D 'part="back"'  mono-note-mini.scad
//   openscad -o front.stl -D 'part="front"' mono-note-mini.scad
//
// part = "assembly" to check fit, "plate" to lay both out for printing.
// =============================================================================

part = "assembly";      // "back" | "front" | "assembly" | "plate"
$fn = 48;

// =============================================================================
// MEASURED INPUTS
//
// !! Every value in this block is an ASSUMPTION until checked with calipers.
// !! Waveshare publishes the outline only as a drawing, so these are inferred
// !! from the 1.54" module and typical dev-board layout. Measure the real board
// !! and correct them here - nothing below needs to change.
// =============================================================================
board_w        = 48.0;  // ASSUMED  PCB width  (X)
board_h        = 40.0;  // ASSUMED  PCB height (Y)
board_t        = 1.6;   // standard 2-layer PCB
board_clear    = 0.35;  // slip fit around the PCB edge

// tallest thing standing on the front face of the PCB (the e-paper module)
front_stack    = 3.2;   // ASSUMED
// tallest thing hanging off the back (TF socket, battery header, speaker)
back_stack     = 6.5;   // ASSUMED

// Display. The active area is exact: 200 px at 0.138 mm pitch.
active         = 27.6;
// Where the centre of the active area sits relative to the PCB centre.
disp_off_x     = 0.0;   // ASSUMED
disp_off_y     = 4.0;   // ASSUMED - display usually sits above centre

// Port and control positions, measured from the PCB's lower-left corner.
usb_x          = 24.0;  // ASSUMED  centre of the USB-C connector, on the bottom edge
usb_w          = 9.5;   // Type-C body + clearance
usb_h          = 3.6;
tf_y           = 20.0;  // ASSUMED  centre of the TF slot, on the right edge
tf_w           = 15.5;
tf_h           = 2.2;
btn_boot_x     = 14.0;  // ASSUMED  BOOT button centre, bottom edge
btn_pwr_x      = 34.0;  // ASSUMED  PWR  button centre, bottom edge
btn_d          = 4.2;   // finger hole over each button

mic_x          = 8.0;   // ASSUMED  microphone port
mic_y          = 34.0;
spk_x          = 24.0;  // ASSUMED  speaker centre (back face)
spk_y          = 12.0;
spk_d          = 14.0;  // grille diameter

// =============================================================================
// SHELL
// =============================================================================
wall           = 2.0;   // side walls
floor_t        = 1.6;   // back face
bezel_t        = 1.6;   // front face
corner_r       = 3.0;   // outside corner radius
lip            = 1.2;   // how far the bezel overlaps the tub wall

// interior the PCB sits in
cav_w          = board_w + 2*board_clear;
cav_h          = board_h + 2*board_clear;

// The PCB rests on a shelf so it cannot press on the back face.
shelf          = 1.5;   // how far the shelf reaches in over the PCB edge
shelf_gap      = back_stack + 0.6;   // clearance under the PCB

body_h         = floor_t + shelf_gap + board_t + front_stack;  // tub height
outer_w        = cav_w + 2*wall;
outer_h        = cav_h + 2*wall;

// =============================================================================
// SNAP FIT
//
// Cantilever hooks on the bezel, catching a groove in the tub wall. Hook length
// is what sets the strain: a 6 mm beam deflecting 0.9 mm stays well inside PLA's
// elastic range, where a 3 mm beam doing the same would whiten and snap.
// =============================================================================
hook_t         = 1.6;   // beam thickness
hook_w         = 7.0;   // beam width along the wall
hook_len       = 6.5;   // free length before the barb
hook_catch     = 0.9;   // engagement depth
hook_lead      = 1.4;   // lead-in chamfer height (assembly ramp)
hook_clear     = 0.15;  // print clearance either side
groove_extra   = 0.25;  // groove cut slightly deeper than the barb

// hook positions: two per long side, one per short side
hook_x = [outer_w*0.28, outer_w*0.72];
hook_y = [outer_h*0.30, outer_h*0.70];

// =============================================================================
// HELPERS
// =============================================================================
module rrect(w, h, r, th) {
    linear_extrude(th) offset(r = r) offset(delta = -r)
        square([w, h], center = false);
}

// A hook pointing inward along +Y at the given position.
module hook(len, thick, wide, catch, lead) {
    union() {
        cube([wide, thick, len]);                       // the beam
        translate([0, 0, len])                          // the barb
            hull() {
                cube([wide, thick, 0.01]);
                translate([0, -catch, 0]) cube([wide, thick + catch, 0.01]);
                translate([0, -catch, lead]) cube([wide, 0.01, 0.01]);
            }
    }
}

module speaker_grille(d) {
    // concentric rings of holes - prints cleanly and stays stiff
    for (ring = [0 : 2]) {
        r = ring * d/6;
        n = ring == 0 ? 1 : ring * 6;
        for (i = [0 : n-1])
            rotate([0, 0, i * 360/n]) translate([r, 0, 0])
                cylinder(d = 1.6, h = 20, center = true);
    }
}

// =============================================================================
// BACK TUB
// =============================================================================
module back_shell() {
    difference() {
        union() {
            // outer body
            rrect(outer_w, outer_h, corner_r, body_h);
        }

        // main cavity, leaving the shelf ledge
        translate([wall, wall, floor_t])
            rrect(cav_w, cav_h, max(0.1, corner_r - wall), body_h);

        // widen above the shelf so the PCB drops in
        translate([wall - shelf, wall - shelf, floor_t + shelf_gap])
            rrect(cav_w + 2*shelf, cav_h + 2*shelf,
                  max(0.1, corner_r - wall + shelf), body_h);

        // ---- port cutouts (through the tub wall, below the PCB top face) ----
        port_z = floor_t + shelf_gap - 0.6;

        // USB-C, bottom edge
        translate([wall + board_clear + usb_x - usb_w/2, -1, port_z])
            cube([usb_w, wall + 2, usb_h + 1.2]);

        // TF card, right edge
        translate([outer_w - wall - 1, wall + board_clear + tf_y - tf_w/2, port_z])
            cube([wall + 2, tf_w, tf_h + 1.2]);

        // BOOT and PWR finger holes, bottom edge
        for (bx = [btn_boot_x, btn_pwr_x])
            translate([wall + board_clear + bx, -1, port_z + btn_d/2 + 0.4])
                rotate([-90, 0, 0]) cylinder(d = btn_d, h = wall + 2);

        // speaker grille through the back face
        translate([wall + board_clear + spk_x, wall + board_clear + spk_y, 0])
            speaker_grille(spk_d);

        // microphone port
        translate([wall + board_clear + mic_x, wall + board_clear + mic_y, -1])
            cylinder(d = 2.0, h = floor_t + 2);

        // ---- snap grooves in the outer wall ----
        gz = body_h - hook_len;
        for (x = hook_x) {
            translate([x - hook_w/2 - hook_clear, -1, gz])
                cube([hook_w + 2*hook_clear, wall - hook_catch + groove_extra + 1, hook_len + 1]);
            translate([x - hook_w/2 - hook_clear,
                       outer_h - (wall - hook_catch + groove_extra), gz])
                cube([hook_w + 2*hook_clear, wall - hook_catch + groove_extra + 1, hook_len + 1]);
        }
        for (y = hook_y) {
            translate([-1, y - hook_w/2 - hook_clear, gz])
                cube([wall - hook_catch + groove_extra + 1, hook_w + 2*hook_clear, hook_len + 1]);
            translate([outer_w - (wall - hook_catch + groove_extra),
                       y - hook_w/2 - hook_clear, gz])
                cube([wall - hook_catch + groove_extra + 1, hook_w + 2*hook_clear, hook_len + 1]);
        }

        // thumb notch, so the two halves can be parted without a tool
        translate([outer_w/2, -1, body_h - 2.5])
            rotate([-90, 0, 0]) cylinder(d = 9, h = wall + 2);
    }
}

// =============================================================================
// FRONT BEZEL
// =============================================================================
module front_frame() {
    win = active + 0.6;                 // aperture, a hair over the active area
    wx = wall + board_clear + board_w/2 + disp_off_x;
    wy = wall + board_clear + board_h/2 + disp_off_y;

    difference() {
        union() {
            rrect(outer_w, outer_h, corner_r, bezel_t);
            // skirt that overlaps the tub
            translate([0, 0, bezel_t]) difference() {
                rrect(outer_w, outer_h, corner_r, lip + hook_len);
                translate([wall - hook_clear, wall - hook_clear, -1])
                    rrect(cav_w + 2*hook_clear, cav_h + 2*hook_clear,
                          max(0.1, corner_r - wall), lip + hook_len + 2);
            }
        }
        // display aperture - must clear the whole active area, the UI uses
        // every row of it including the bottom bar
        translate([wx - win/2, wy - win/2, -1]) cube([win, win, bezel_t + 2]);
    }

    // hooks, pointing outward into the tub grooves
    for (x = hook_x) {
        translate([x - hook_w/2, wall - hook_t, bezel_t])
            hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
        translate([x + hook_w/2, outer_h - wall + hook_t, bezel_t])
            rotate([0, 0, 180])
                hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
    }
    for (y = hook_y) {
        translate([wall - hook_t, y + hook_w/2, bezel_t])
            rotate([0, 0, -90])
                hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
        translate([outer_w - wall + hook_t, y - hook_w/2, bezel_t])
            rotate([0, 0, 90])
                hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
    }
}

// =============================================================================
// OUTPUT
// =============================================================================
module pcb_ghost() {
    color("green", 0.35)
        translate([wall + board_clear, wall + board_clear, floor_t + shelf_gap])
            cube([board_w, board_h, board_t]);
}

if (part == "back")  back_shell();
else if (part == "front") front_frame();
else if (part == "plate") {
    back_shell();
    translate([outer_w + 6, 0, 0]) front_frame();
} else {
    back_shell();
    pcb_ghost();
    // bezel flipped and dropped on top
    color("white", 0.6)
        translate([0, outer_h, body_h + bezel_t + lip]) rotate([180, 0, 0])
            mirror([0, 1, 0]) front_frame();
}

echo(str("outer size: ", outer_w, " x ", outer_h, " x ", body_h + bezel_t, " mm"));
