// Mono Note Mini - snap-fit enclosure, no screws
// =============================================================================
// A replacement for the case the board ships in. The stock one is a two-part
// shell closed with two screws; this is the same external envelope closed with
// six cantilever hooks instead, so it opens with a thumbnail.
//
//   openscad -o back.stl  -D 'part="back"'  mono-note-mini.scad
//   openscad -o front.stl -D 'part="front"' mono-note-mini.scad
//
// part = "assembly" to check fit, "plate" to lay both out for printing.
// =============================================================================

part = "assembly";      // "back" | "front" | "assembly" | "plate"
$fn = 64;

// =============================================================================
// KNOWN - from Waveshare's outline drawing for ESP32-S3-ePaper-1.54
// (ESP32-S3-ePaper-1.54-details-size.jpg). These are the dimensions of the
// finished product in its stock case, so matching them keeps the device the
// same size in the hand and keeps the window over the glass.
// =============================================================================
case_w      = 39.80;    // width
case_h      = 53.00;    // height - the device is portrait, taller than wide
case_d      = 16.90;    // total thickness
case_r      = 4.50;     // outside corner radius

win         = 27.80;    // display window, square
win_bottom  = 14.30;    // from the bottom edge up to the bottom of the window
// horizontally centred: (39.80 - 27.80) / 2 = 6.00 each side

// For reference: the glass itself is 200 px at 0.138 mm pitch = 27.6 mm, so
// Waveshare's 27.80 window already carries 0.1 mm of margin per side.

// =============================================================================
// ASSUMED - not published anywhere. Waveshare dimensions the cased product,
// never the bare PCB, so everything here waits on calipers. See README.
// =============================================================================
pcb_w       = 34.60;    // ASSUMED  from the 22.00 + 12.60 chain on the drawing
pcb_h       = 34.00;    // ASSUMED  the labelled PCB area on the back view
pcb_t       = 1.6;
pcb_bottom  = 9.50;     // ASSUMED  from the drawing: case bottom to PCB bottom
pcb_clear   = 0.35;

front_stack = 3.4;      // ASSUMED  e-paper module standing off the PCB
back_stack  = 7.0;      // ASSUMED  TF socket / battery header / speaker

// Side buttons. The drawing's side view shows BOOT and PWR on one edge, but
// does not dimension them - these are placeholders.
btn_side    = "left";   // ASSUMED  which edge the buttons sit on
btn_boot_z  = 34.0;     // ASSUMED  height up the case
btn_pwr_z   = 25.0;     // ASSUMED
btn_d       = 4.0;

usb_x       = case_w/2; // ASSUMED  USB-C centred on the bottom edge
usb_w       = 9.6;
usb_t       = 3.6;
tf_z        = 40.0;     // ASSUMED  TF slot height on the right edge
tf_w        = 15.5;
tf_t        = 2.2;

mic_x       = 12.0;     // ASSUMED  mic port, bottom area of the front face
mic_y       = 7.0;
spk_x       = case_w/2; // ASSUMED  speaker grille on the back
spk_y       = 9.0;
spk_d       = 15.0;

// =============================================================================
// SHELL
// =============================================================================
wall        = 2.0;
bezel_t     = 1.8;      // front face thickness
floor_t     = 1.8;      // back face thickness
lip         = 1.5;      // bezel skirt overlap onto the tub

tub_d       = case_d - bezel_t;         // tub height, so the pair totals case_d
cav_w       = case_w - 2*wall;
cav_h       = case_h - 2*wall;

// =============================================================================
// SNAP FIT
//
// Hooks on the bezel, grooves in the tub. Free length is what keeps them alive:
// a 6.5 mm beam taking 0.9 mm of deflection stays inside PLA's elastic range,
// where a short stubby hook doing the same job whitens and breaks.
// =============================================================================
hook_t      = 1.5;
hook_w      = 6.0;
hook_len    = 6.5;
hook_catch  = 0.85;
hook_lead   = 1.4;
hook_clear  = 0.15;
groove_over = 0.25;

hook_x = [case_w*0.30, case_w*0.70];    // two on each short edge
hook_y = [case_h*0.28, case_h*0.72];    // two on each long edge

// =============================================================================
// HELPERS
// =============================================================================
module rrect(w, h, r, th) {
    linear_extrude(th) offset(r = r) offset(delta = -r) square([w, h]);
}

module hook(len, thick, wide, catch, lead) {
    union() {
        cube([wide, thick, len]);
        translate([0, 0, len]) hull() {
            cube([wide, thick, 0.01]);
            translate([0, -catch, 0]) cube([wide, thick + catch, 0.01]);
            translate([0, -catch, lead]) cube([wide, 0.01, 0.01]);
        }
    }
}

module grille(d) {
    for (ring = [0 : 2]) {
        r = ring * d/6;
        n = ring == 0 ? 1 : ring * 6;
        for (i = [0 : n-1])
            rotate([0, 0, i * 360/n]) translate([r, 0, 0])
                cylinder(d = 1.5, h = 30, center = true);
    }
}

// Grooves are cut from the tub as a set, so back and front stay in step.
module groove_cuts() {
    gz = tub_d - hook_len;
    for (x = hook_x) {
        translate([x - hook_w/2 - hook_clear, -1, gz])
            cube([hook_w + 2*hook_clear, wall - hook_catch + groove_over + 1, hook_len + 2]);
        translate([x - hook_w/2 - hook_clear, case_h - (wall - hook_catch + groove_over), gz])
            cube([hook_w + 2*hook_clear, wall - hook_catch + groove_over + 1, hook_len + 2]);
    }
    for (y = hook_y) {
        translate([-1, y - hook_w/2 - hook_clear, gz])
            cube([wall - hook_catch + groove_over + 1, hook_w + 2*hook_clear, hook_len + 2]);
        translate([case_w - (wall - hook_catch + groove_over), y - hook_w/2 - hook_clear, gz])
            cube([wall - hook_catch + groove_over + 1, hook_w + 2*hook_clear, hook_len + 2]);
    }
}

// =============================================================================
// BACK TUB
// =============================================================================
module back_shell() {
    pcb_z = floor_t + back_stack;       // underside of the PCB
    difference() {
        rrect(case_w, case_h, case_r, tub_d);

        // interior
        translate([wall, wall, floor_t])
            rrect(cav_w, cav_h, max(0.1, case_r - wall), tub_d);

        // USB-C on the bottom edge
        translate([usb_x - usb_w/2, -1, pcb_z - usb_t/2])
            cube([usb_w, wall + 2, usb_t + 1.0]);

        // TF slot on the right edge
        translate([case_w - wall - 1, tf_z - tf_w/2, pcb_z - tf_t/2])
            cube([wall + 2, tf_w, tf_t + 1.0]);

        // side buttons
        bx = (btn_side == "left") ? -1 : case_w - wall - 1;
        for (z = [btn_boot_z, btn_pwr_z])
            translate([bx, z, pcb_z + 1.5]) rotate([0, 90, 0])
                cylinder(d = btn_d, h = wall + 2);

        // speaker grille through the back
        translate([spk_x, spk_y, 0]) grille(spk_d);

        groove_cuts();

        // thumb notch so the halves part without a tool
        translate([case_w/2, -1, tub_d - 2.5]) rotate([-90, 0, 0])
            cylinder(d = 10, h = wall + 2);
    }

    // PCB shelf: four posts rather than a continuous ledge, so the board can be
    // dropped in past the connectors on its edges
    for (p = [[wall + 2.5, wall + 2.5], [case_w - wall - 2.5, wall + 2.5],
              [wall + 2.5, case_h - wall - 2.5], [case_w - wall - 2.5, case_h - wall - 2.5]])
        translate([p[0], p[1], floor_t]) cylinder(d = 3.6, h = back_stack);
}

// =============================================================================
// FRONT BEZEL
// =============================================================================
module front_frame() {
    wx = (case_w - win) / 2;
    difference() {
        union() {
            rrect(case_w, case_h, case_r, bezel_t);
            translate([0, 0, bezel_t]) difference() {           // skirt
                rrect(case_w, case_h, case_r, lip + hook_len);
                translate([wall - hook_clear, wall - hook_clear, -1])
                    rrect(cav_w + 2*hook_clear, cav_h + 2*hook_clear,
                          max(0.1, case_r - wall), lip + hook_len + 2);
            }
        }
        // Display aperture. The whole window is exposed on purpose - the UI
        // puts its back button in the bottom rows of the panel, so a bezel
        // lapping even 2 mm over the glass would sit on a control.
        translate([wx, win_bottom, -1]) cube([win, win, bezel_t + 2]);

        // microphone port
        translate([mic_x, mic_y, -1]) cylinder(d = 1.8, h = bezel_t + 2);
    }

    for (x = hook_x) {
        translate([x - hook_w/2, wall - hook_t, bezel_t])
            hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
        translate([x + hook_w/2, case_h - wall + hook_t, bezel_t]) rotate([0, 0, 180])
            hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
    }
    for (y = hook_y) {
        translate([wall - hook_t, y + hook_w/2, bezel_t]) rotate([0, 0, -90])
            hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
        translate([case_w - wall + hook_t, y - hook_w/2, bezel_t]) rotate([0, 0, 90])
            hook(hook_len + lip, hook_t, hook_w, hook_catch, hook_lead);
    }
}

// =============================================================================
// OUTPUT
// =============================================================================
module pcb_ghost() {
    color("green", 0.35)
        translate([(case_w - pcb_w)/2, pcb_bottom, floor_t + back_stack])
            cube([pcb_w, pcb_h, pcb_t]);
}

if (part == "back") back_shell();
else if (part == "front") front_frame();
else if (part == "plate") { back_shell(); translate([case_w + 6, 0, 0]) front_frame(); }
else {
    back_shell();
    pcb_ghost();
    color("white", 0.55)
        translate([0, case_h, tub_d + bezel_t + lip]) rotate([180, 0, 0]) mirror([0, 1, 0])
            front_frame();
}

echo(str("outside: ", case_w, " x ", case_h, " x ", tub_d + bezel_t, " mm"));
