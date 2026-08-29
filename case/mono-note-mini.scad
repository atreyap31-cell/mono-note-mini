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
case_r      = 4.50;     // kept for reference; the profile is a squircle now

// Squircle: |x/a|^n + |y/b|^n = 1. n=4 is the definition; n=2 would be an
// ellipse and n->inf a rectangle. Apple's icon is n~5 and not strictly a
// squircle. The corners stay curvature-continuous, so there is no point where
// a straight edge meets an arc - which is the whole visual point of it.
sq_n        = 4;
sq_steps    = 160;      // polygon resolution around the profile

// The glass is the number that matters: 200 px at 0.138 mm pitch. Waveshare's
// own window is 27.80 at 14.30, i.e. centred on the glass with 0.1 mm a side.
glass        = 27.60;
glass_bottom = 14.40;   // keeps the same centre as Waveshare's 27.80 @ 14.30

// The aperture is cut wider than the glass on purpose. Where the display sits
// in the case is an assumed figure, and at 0.1 mm a side any error at all puts
// the bezel over live pixels - which would eat the back button, since the UI
// draws it in the bottom rows of the panel. Losing a little of the module's
// black border costs nothing; covering a control costs a control.
win_margin  = 0.60;
win         = glass + 2*win_margin;             // 28.80
win_bottom  = glass_bottom - win_margin;        // 13.80

// =============================================================================
// ASSUMED - not published anywhere. Waveshare dimensions the cased product,
// never the bare PCB, so everything here waits on calipers. See README.
// =============================================================================
pcb_w       = 34.60;    // ASSUMED  from the 22.00 + 12.60 chain on the drawing
pcb_h       = 34.00;    // ASSUMED  the labelled PCB area on the back view
pcb_t       = 1.6;
pcb_bottom  = 9.50;     // ASSUMED  from the drawing: case bottom to PCB bottom
pcb_clear   = 0.35;
pcb_squeeze = 0.10;     // how hard the bezel rails press the board down

// derived footprint - posts below and rails above both key off these
pcb_x0 = (case_w - pcb_w)/2;   pcb_x1 = pcb_x0 + pcb_w;
pcb_y0 = pcb_bottom;           pcb_y1 = pcb_y0 + pcb_h;

front_stack = 3.4;      // ASSUMED  e-paper module standing off the PCB
back_stack  = 7.0;      // ASSUMED  TF socket / battery header / speaker

// Side buttons. The drawing's side view shows BOOT and PWR on one edge, but
// does not dimension them - these are placeholders.
btn_side    = "left";   // ASSUMED  which edge the buttons sit on
// Kept low and close together: on a squircle the side is only truly flat
// through the middle of its run, and a flexure on a curved patch binds.
btn_boot_z  = 22.0;     // ASSUMED  height up the case
btn_pwr_z   = 13.0;     // ASSUMED
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
// Declared here, not up with the PCB block: OpenSCAD resolves top-level
// assignments in file order, so referencing floor_t before this point silently
// yields undef - which is exactly how the hold-down rails went missing once.
pcb_top_z   = floor_t + back_stack + pcb_t;
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
rail_w      = 1.2;      // hold-down rail width
rail_gap    = 0.05;     // keeps the rail off the display window

fin_t       = 1.6;      // finger thickness
fin_w       = 5.0;      // finger width - see fin_x
fin_len     = 9.0;      // free length - what keeps the strain low
fin_catch   = 0.8;      // how far the barb reaches past the cavity face
fin_lead    = 1.5;      // assembly ramp height
fin_clear   = 0.2;      // groove clearance around the finger
fin_root    = 1.0;      // gusset at the base, inward face only

// 0.4 mm nozzle at a 0.40 mm line width: the 1.6 mm finger is exactly four
// walls and the 2.0 mm shell exactly five, so neither gets a sliver of infill
// down its middle. A finger with infill in it snaps.

// Guards. A dimension that resolves to undef only warns, and the part exports
// looking fine with the feature quietly absent.
assert(pcb_top_z != undef && pcb_top_z > 0, "pcb_top_z undefined - check declaration order");
assert(tub_d - pcb_top_z > 0, "no room between the board and the bezel");
assert((case_w - win)/2 > wall, "display aperture is wider than the shell allows");

// Held close to the centre of the edge, where the squircle is flattest. Out at
// 0.32/0.68 of the width the wall has fallen away 0.8 mm across a 7 mm finger,
// which takes the barb's engagement to zero at its inner end - it would simply
// pop open. At +/-6 mm with a 5 mm finger the wall moves 0.31 mm and the barb
// keeps 0.49-0.80 mm of grip along its whole width.
fin_x = [case_w/2 - 6, case_w/2 + 6];
catch_z = tub_d - fin_len;              // where the barb lands, both parts agree

// Where a finger's outer face has to sit so it clears the curved cavity wall
// across its whole width. On a straight wall this was just wall + fit_clear;
// on a squircle the wall falls away toward the corners, and a fixed inset
// would bury the finger's outer end in the shell.
function fin_face(fx) =
    let(a = cav_w/2, b = cav_h/2, cy = case_h/2, cx = case_w/2)
    cy - min(sq_reach(fx - fin_w/2 - cx, a, b, sq_n),
             sq_reach(fx + fin_w/2 - cx, a, b, sq_n)) + fit_clear;

// =============================================================================
// SIDE ENGRAVING
// =============================================================================
// The long word goes on whichever side has no buttons; the short one fits in
// the clear run below them. Both sit low on the wall, under the TF slot and
// under the button tongues, so nothing collides.
eng_long    = "mono note";
eng_short   = "MINI";
eng_font    = "Liberation Sans:style=Bold";
eng_size    = 3.0;              // cap height
eng_depth   = 0.5;              // how deep it cuts
eng_z       = 4.5;              // low on the wall, below every cutout
// The squircle's long side only runs flat through its middle - it falls away
// by 0.5 mm near the ends, which would swallow a 0.5 mm engraving. Both words
// sit in the flat band: y 15-38 varies by under 0.2 mm.
eng_y_btn   = 33.0;             // button side: above both tongues
eng_y_plain = 23.0;             // clear side: below the TF slot

// =============================================================================
// BUTTONS
//
// Flexure tongues cut straight out of the side wall rather than loose caps:
// nothing to lose, nothing to assemble, and no gap for dust. The wall is thinned
// behind each tongue so it can actually bend - 2 mm of PETG will not.
// =============================================================================
btn_tongue_l = 7.5;     // along the case height
btn_tongue_w = 5.0;     // along the case depth
btn_tongue_t = 1.0;     // thinned wall at the tongue
btn_slot     = 0.6;     // gap around it - printable at a 0.4 nozzle
btn_dish_d   = 4.2;     // finger dish sunk into the tongue
btn_dish_h   = 0.45;    // a proud pad would push the case past 39.80 wide
btn_boss_l   = 1.8;     // ASSUMED  inward reach to the switch
btn_boss_d   = 2.6;
btn_z        = floor_t + back_stack + pcb_t/2 + 1.0;   // level with the switches

// =============================================================================
// HELPERS
// =============================================================================
// Superellipse profile, drawn from 0,0 to w,h. `r` is ignored - the corner is
// set by sq_n, not by a radius - but the parameter stays so every call site
// reads the same.
function sq_pt(t, w, h, n) = [
    w/2 + (w/2) * sign(cos(t)) * pow(abs(cos(t)), 2/n),
    h/2 + (h/2) * sign(sin(t)) * pow(abs(sin(t)), 2/n)
];
module rrect(w, h, r, th) {
    linear_extrude(th)
        polygon([for (i = [0 : sq_steps - 1]) sq_pt(i * 360 / sq_steps, w, h, sq_n)]);
}

// How far the profile reaches from its centre line, at a given offset along the
// other axis. Used to sit the snap fingers against a wall that is no longer
// straight - placing them at a fixed inset would bury them in the shell.
function sq_reach(d, a, b, n) = b * pow(max(0, 1 - pow(abs(d/a), n)), 1/n);

// A cantilever finger rising in +Z, its barb reaching out in -Y. `over` is how
// far the barb passes the finger's own outer face.
//
// Printed upright the layer lines run across the beam, so bending it works the
// layer bonds rather than the plastic - that is where a printed snap breaks,
// always at the root. The gusset spreads that load. It flares inward only: the
// outward face has just fit_clear between it and the tub wall.
module finger(len, thick, wide, over, lead) {
    union() {
        cube([wide, thick, len]);
        hull() {
            cube([wide, thick + fin_root, 0.01]);
            translate([0, 0, fin_root]) cube([wide, thick, 0.01]);
        }
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

// Text sunk into a long side. left = -X face, right = +X face.
module side_engraving(txt, on_left, ey) {
    // Starts outside the shell and cuts inward, so it still bites where the
    // squircle has pulled the surface in. Depth therefore varies slightly along
    // the word - about 0.2 mm across the flat band, out of 0.5 mm.
    e = eng_depth + 0.8;
    if (on_left)
        translate([eng_depth, ey, eng_z]) rotate([90, 0, -90])
            linear_extrude(e) text(txt, size = eng_size, font = eng_font,
                                   halign = "center", valign = "center");
    else
        translate([case_w - eng_depth, ey, eng_z]) rotate([90, 0, 90])
            linear_extrude(e) text(txt, size = eng_size, font = eng_font,
                                   halign = "center", valign = "center");
}

// One flexure tongue in a side wall, anchored at its lower end. `by` is the
// centre along the case height. Cuts and additions are separate so the caller
// can difference then union in the right order.
module button_cuts(by, on_left) {
    x0 = on_left ? -1 : case_w - wall - 1;
    tl = btn_tongue_l; tw = btn_tongue_w; s = btn_slot;
    // U-slot: two rails along the height, one across the free end
    for (dz = [-1, 1])
        translate([x0, by - tl/2, btn_z + dz*(tw/2 + s/2) - s/2])
            cube([wall + 2, tl + s, s]);
    translate([x0, by + tl/2, btn_z - tw/2 - s])
        cube([wall + 2, s, tw + 2*s]);
    // shallow dish on the outside so a fingertip finds the button
    xd = on_left ? -0.01 : case_w - btn_dish_h;
    translate([xd, by + btn_tongue_l/6, btn_z]) rotate([0, 90, 0])
        cylinder(d = btn_dish_d, h = btn_dish_h + 0.01);
    // thin the wall behind the tongue so it can bend
    xr = on_left ? btn_tongue_t : wall - btn_tongue_t;
    translate([xr, by - tl/2, btn_z - tw/2])
        cube([wall - btn_tongue_t + 0.01, tl, tw]);
}

module button_adds(by, on_left) {
    // inner boss only - the dish on the outside is a cut, see button_cuts
    xb = on_left ? btn_tongue_t : case_w - btn_tongue_t;
    translate([xb, by + btn_tongue_l/6, btn_z]) rotate(on_left ? [0, 90, 0] : [0, -90, 0])
        cylinder(d = btn_boss_d, h = btn_boss_l);
}

// Grooves in the tub's inner wall, cut from the cavity face outward. Both parts
// derive their position from catch_z, so they cannot drift apart.
module groove_cuts() {
    gw = fin_w + 2*fin_clear;
    gh = fin_lead + 1.6;                    // a little taller than the barb
    for (x = fin_x) {
        // Cut from below the barb tip up to the finger face. Material only
        // exists outboard of the curved cavity wall, so the groove ends up
        // following that curve without having to be modelled as one.
        gd = fin_catch + fit_clear + 0.15;
        translate([x - gw/2, fin_face(x) - gd, catch_z - 0.4])           // bottom edge
            cube([gw, gd + 0.01, gh]);
        translate([x - gw/2, case_h - fin_face(x) - 0.01, catch_z - 0.4])
            cube([gw, gd + 0.01, gh]);                                    // top edge
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

        // side buttons - flexure tongues cut out of the wall
        for (by = [btn_boot_z, btn_pwr_z])
            button_cuts(by, btn_side == "left");

        // engraving. On the button side it drops below them; the other side
        // has the full run and takes it centred.
        left_has_btn = (btn_side == "left");
        side_engraving(left_has_btn ? eng_short : eng_long, true,
                       left_has_btn ? eng_y_btn : eng_y_plain);
        side_engraving(left_has_btn ? eng_long : eng_short, false,
                       left_has_btn ? eng_y_plain : eng_y_btn);

        // speaker grille through the back
        translate([spk_x, spk_y, 0]) grille(spk_d);

        groove_cuts();

        // thumb notch so the halves part without a tool
        translate([case_w/2, -1, tub_d - 2.5]) rotate([-90, 0, 0])
            cylinder(d = 10, h = wall + 2);
    }

    // Posts under the board's corners. They were previously at the cavity
    // corners, which is 5 mm clear of the PCB at both ends - the board rested
    // on nothing at all.
    for (p = [[pcb_x0 + 3, pcb_y0 + 3], [pcb_x1 - 3, pcb_y0 + 3],
              [pcb_x0 + 3, pcb_y1 - 3], [pcb_x1 - 3, pcb_y1 - 3]])
        translate([p[0], p[1], floor_t]) cylinder(d = 3.6, h = back_stack);

    // finger pads and inner bosses, added after the wall has been cut
    for (by = [btn_boot_z, btn_pwr_z])
        button_adds(by, btn_side == "left");
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

    // Hold-down rails. Without these the board only rests on posts and can
    // lift 2.7 mm inside a closed case - it rattles, and the display is not
    // held against the window. The rails run down each side of the PCB in the
    // margin between the display window and the board edge, so the board is
    // clamped between post and rail with nothing crossing the screen.
    rail_len = tub_d - pcb_top_z + pcb_squeeze;
    wx0 = (case_w - win)/2;
    for (rx = [wx0 - rail_w - rail_gap, wx0 + win + rail_gap])
        translate([rx, pcb_y0 + 2, bezel_t])
            cube([rail_w, (pcb_y1 - pcb_y0) - 4, rail_len]);

    // Fingers, on the short edges only. Barbs face outward into the grooves.
    for (x = fin_x) {
        translate([x - fin_w/2, fin_face(x), bezel_t])              // bottom edge
            finger(fin_len, fin_t, fin_w, fin_catch + fit_clear, fin_lead);
        translate([x + fin_w/2, case_h - fin_face(x), bezel_t]) rotate([0, 0, 180])
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
