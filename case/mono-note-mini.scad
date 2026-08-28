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

tub_d       = case_d - bezel_t;         // tub height, so the pair totals case_d
cav_w       = case_w - 2*wall;
cav_h       = case_h - 2*wall;

// =============================================================================
// SNAP FIT
//
// The bezel plugs into the tub rather than capping it, so the outside stays a
// smooth 39.80 x 53.00 with nothing protruding. Four cantilever fingers hang
// from the bezel and catch grooves in the tub's inner wall.
//
// They sit on the short edges only, and that is forced by the board: the PCB is
// ~34.6 mm wide in a 35.8 mm cavity, leaving 0.6 mm at each long side - no room
// for a finger. Above and below the board there is ~7.5 mm to work in, which
// buys a 9 mm cantilever. A finger short enough to fit beside the PCB would be
// the stubby kind that stress-whitens and snaps after a few openings.
// =============================================================================
fit_clear   = 0.25;     // plug to cavity, each side
rim_h       = 2.0;      // locating rim depth into the tub

fin_t       = 1.6;      // finger thickness
fin_w       = 7.0;      // finger width
fin_len     = 9.0;      // free length - what keeps the strain low
fin_catch   = 0.8;      // how far the barb reaches past the cavity face
fin_lead    = 1.5;      // assembly ramp height
fin_clear   = 0.2;      // groove clearance around the finger

fin_x = [case_w*0.32, case_w*0.68];     // two per short edge
catch_z = tub_d - fin_len;              // where the barb lands, both parts agree

// =============================================================================
// HELPERS
// =============================================================================
// Rounded box as a hull of four cylinders. The offset(r)/offset(-r) idiom is
// tidier to read but hands CGAL near-degenerate arcs, which it fails on.
module rrect(w, h, r, th) {
    rr = min(r, w/2 - 0.01, h/2 - 0.01);
    hull() for (p = [[rr, rr], [w - rr, rr], [rr, h - rr], [w - rr, h - rr]])
        translate([p[0], p[1], 0]) cylinder(r = rr, h = th);
}

// A cantilever finger rising in +Z, its barb reaching out in -Y. `over` is how
// far the barb passes the finger's own outer face.
module finger(len, thick, wide, over, lead) {
    union() {
        cube([wide, thick, len]);
        hull() {                                  // barb at the free end
            translate([0, 0, len - lead - 0.6]) cube([wide, thick, 0.01]);
            translate([0, -over, len - lead]) cube([wide, thick + over, 0.01]);
            translate([0, -over, len]) cube([wide, over + 0.01, 0.01]);
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

// Grooves in the tub's inner wall, cut from the cavity face outward. Both parts
// derive their position from catch_z, so they cannot drift apart.
module groove_cuts() {
    gw = fin_w + 2*fin_clear;
    gh = fin_lead + 1.6;                    // a little taller than the barb
    for (x = fin_x) {
        translate([x - gw/2, wall - fin_catch, catch_z - 0.4])          // bottom edge
            cube([gw, fin_catch + 0.01, gh]);
        translate([x - gw/2, case_h - wall - 0.01, catch_z - 0.4])      // top edge
            cube([gw, fin_catch + 0.01, gh]);
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
    // plug footprint: the cavity, less a slip fit
    pw = cav_w - 2*fit_clear;
    ph = cav_h - 2*fit_clear;
    px = wall + fit_clear;

    difference() {
        union() {
            rrect(case_w, case_h, case_r, bezel_t);
            // shallow locating rim, so the bezel seats square before the
            // fingers engage
            translate([px, px, bezel_t])
                rrect(pw, ph, max(0.1, case_r - wall), rim_h);
        }
        // Display aperture. The whole window is exposed on purpose - the UI
        // puts its back button in the bottom rows of the panel, so a bezel
        // lapping even 2 mm over the glass would sit on a control.
        translate([wx, win_bottom, -1]) cube([win, win, bezel_t + rim_h + 2]);

        // hollow the rim out so it does not foul the display module
        translate([px + 2, px + 2, bezel_t])
            rrect(pw - 4, ph - 4, max(0.1, case_r - wall - 2), rim_h + 1);

        // microphone port
        translate([mic_x, mic_y, -1]) cylinder(d = 1.8, h = bezel_t + 2);
    }

    // Fingers, on the short edges only. Barbs face outward into the grooves.
    for (x = fin_x) {
        translate([x - fin_w/2, px, bezel_t])                       // bottom edge
            finger(fin_len, fin_t, fin_w, fin_catch + fit_clear, fin_lead);
        translate([x + fin_w/2, case_h - px, bezel_t]) rotate([0, 0, 180])
            finger(fin_len, fin_t, fin_w, fin_catch + fit_clear, fin_lead);
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
    // flipped onto the tub: rotate alone is the whole transform - adding a
    // mirror as well cancels the Y flip and shifts the part a case-length away
    color("white", 0.55)
        translate([0, case_h, tub_d + bezel_t]) rotate([180, 0, 0])
            front_frame();
}

echo(str("outside: ", case_w, " x ", case_h, " x ", tub_d + bezel_t, " mm"));
