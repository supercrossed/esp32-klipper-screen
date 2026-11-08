# Case design notes

For printing and assembly, see the **[build guide](README.md)**. This page is for changing the design.

This is a desk case for the screen in landscape, leaning back 20°. It splits into two halves so that each one prints without supports. The **front frame** holds the display. The **body** holds the battery and has the USB-C slot and the button guides. A magnetic **bezel** sits flush in the front. It covers the glass border and hides the screws that join the two halves.

The Fusion script `fusion/EspP4Case/EspP4Case.py` builds every part. Each run creates a new design and writes these files to `export/`:

| File | Part | Print orientation |
|---|---|---|
| `esp32-p4-case-front-frame.3mf` | Front frame | Face up, back on the bed |
| `esp32-p4-case-body.3mf` | Body | Standing on its closed back |
| `esp32-p4-case-bezel.3mf` | Magnetic bezel | Face up, so its rounded front edges print cleanly |
| `esp32-p4-case-brackets.3mf` | 4 display brackets | Flat plate on the bed, bosses up |
| `esp32-p4-case-buttons.3mf` | 3 button plungers (D-profile) | Lying on the flat side |
| `esp32-p4-case-assembly.step` | Full assembly with reference display | — |

None of the parts needs supports. In these print orientations the only overhangs are short bridges:
- On the back of the front frame: a 30 mm bridge over the USB notch, plus three small button channels.
- On the back of the bezel: the roofs of the four 6 mm magnet pockets.

### Rounding
The outside is rounded, and every radius was chosen so the part still prints without supports:
- The four long edges, where the sides meet the top and the floor, have a 6 mm radius. The walls are 3 mm thick with matching inside radii, so they don't thin out at the corners.
- The face perimeter has a 2 mm roll.
- The bezel recess corners follow the outer radius, so the rim stays an even width.
- The bezel is 3 mm thick and has rounded corners. Its window has rounded corners and a 2 mm 45° chamfer on the front edge, so a finger can reach the edges of the touch area. A 1 mm flat land stays against the glass. The outer front edge of the bezel is left square so it sits crisply in the rim.
- The back edges have a 3 mm 45° chamfer rather than a fillet. When the body prints, these edges sit on the bed, and a fillet there would droop. A 4 mm fillet (`BACK_BLEND_R`) blends the top of the chamfer into the walls all the way round. It's never steeper than the chamfer's own 45°, so it still prints without supports.

Each of these is a parameter (`EDGE_R`, `FACE_R`, `BACK_CHAMFER`, `BACK_BLEND_R`, `WINDOW_CHAMFER`, `WINDOW_R`). Keep `FACE_R` below `RIM`, and keep `WINDOW_CHAMFER` at least 0.8 mm less than `BEZEL_T`.

## Test-print corrections
These changes came from the first test print. Each one is a parameter, so it's easy to adjust:
- **Buttons** (`BTN_SHIFT_X = -5.0`): moved 5 mm towards the left edge, as seen from the front. Flip the sign if they need to go the other way.
- **Body depth** (`DEPTH = 53.0`, was 50): the top brackets now clear the back wall by about 3.5 mm.
- **Bracket reliefs** (`BRK_RELIEF = 1.0`): pockets cut into the inside of the body walls around each bracket, giving 1 mm of clearance all round. The walls there are about 2 mm thick.
- **Bezel recess** (`RECESS_EXTRA = 0.5`): the recess in the front frame is 0.5 mm larger per side, so the bezel has 0.7 mm of clearance per side. The bezel itself is unchanged.

### Buttons
The plungers have a slight D-profile: there's a flat 1.9 mm from the axis (`BTN_FLAT`), so they print lying down and come out with round, smooth shafts. The holes in the body are matching D-shapes with 0.25 mm of clearance, so the buttons can't rotate. The flat faces the front of the case, which puts it on top of the hole when the body prints standing on its back. That top prints as a short 3.4 mm bridge. The nub that presses the switch is 4 mm across (`BTN_NUB_D`) so it reaches the print flat. The shafts run up the 20° tilt of the face, but each cap is cut level, parallel to the top of the case, and stands 1.5 mm above it (`BTN_PROUD`).

## Build guide images
The [build guide](README.md) is illustrated with the `export/build-NN-*.png` renders.

The generator also models the heat-set inserts, screws, magnets and an 18650 battery in place, in the `HW …` and `REF Battery` components. These are only used for the renders and the STEP file; they're never exported as print files. To regenerate the images:
1. Run `EspP4Case.py` in Fusion.
2. Run `fusion/EspP4Case/render_build_guide.py` in Fusion, with the generated design active. This colours the parts, stages each step as an exploded view and saves the PNGs. It also writes the screen position and a visibility mask for each final-assembly shot to `export/_render/`, which git ignores.
3. Run `python fusion/EspP4Case/overlay_screens.py` from a normal terminal. You'll need Pillow and numpy. This puts real UI screenshots from `docs/images/` onto the display in the finished shots. The UI is perspective-warped to the screen and kept behind the bezel edge. To pick different screenshots, change the image names in the `screen_shot(...)` calls in `render_build_guide.py`.

## Run it
In Fusion, open **Utilities → Scripts and Add-Ins**, click **+**, choose **Script or add-in from device** and pick `case/fusion/EspP4Case`. Then click **Run**. A message box shows the case size, the interference check results and the list of exported files.

## Measure these before printing
The Waveshare drawing gives no thickness data, so these parameters at the top of the script are estimates:

- `GLASS_T` (5.0): thickness of the glass and LCD together at the glass edge. This sets the pocket depth. The glass should sit `GLASS_PROUD` (0.2) above the recess floor so the bezel clamps it.
- `STACK` (16.0): distance from the glass front to the tip of the deepest part on the back, such as the header pins.
- `PCB_OFF_LEFT` (7.5): seen from the front, how far the left edge of the PCB is inset from the left edge of the glass.
- `MOUNT_INSET_X_USB`, `MOUNT_INSET_X_OTHER`, `MOUNT_INSET_Y` (3.25 / 3.0 / 3.75): where the PCB mounting holes are.
- `USB_Z_BEHIND_GLASS` (3.2), `BTN_Z_BEHIND_GLASS` (3.4): how far behind the back of the glass the USB-C and button centres sit.
- `BTN_SWITCH_PROTRUDE` (0.8): how far the switch actuators stick out past the top edge of the PCB.

The buttons assume the three switches are side-push types that press down from the top edge.

### Display mounting
The board's four corner screw mounts end 12 mm behind the glass front (`MOUNT_DEPTH`, measured). Each of the four flat brackets sits directly on the ends of those mounts. That leaves 5.4 mm of clearance between the bracket plates and the PCB for components. Each bracket also screws into the back of the front frame.

The screws thread into the board's own mounts from behind. The front of the PCB sits against the LCD, so there's no other way in.

The 3.4 mm clearance holes take M3 or M2.5 screws. The script only adds a boss under the plate if `MOUNT_DEPTH` is set shallower than the PCB back plus `BRK_GAP`.

## Hardware details
- 8× 6×2 mm magnets: 4 in the frame recess and 4 in the bezel. Glue them in so each pair attracts. The pockets are 2.2 mm deep, with 0.8 mm of skin on the bezel's front face. To use 6×3 magnets, set `MAG_H = 3.0` and `BEZEL_T = 4.0`.
- 4× M3 heat-set inserts, 5 mm OD, up to 9 mm long, for the body's joint columns. The holes are 4.4 mm wide and 10 mm deep (`JOINT_INSERT_D`, `JOINT_INSERT_L`).
- 4× M3×10 or M3×12 countersunk screws to join the frame to the body. They go in from the front, under the bezel, and thread into the inserts. The frame is 6 mm thick at the screws, so M3×10 gives about 4 mm of thread in the insert.
- 4× M3×6 or M2.5×6 screws, whichever the board's mounts take, to fix the brackets to the PCB. The plate is 3 mm thick, so about 3 mm of thread goes into the mount.
- 4× short M3 heat-set inserts (5 mm OD, **4 mm long**) for the back of the front frame. The frame is only 6 mm thick at these points, so the holes are 4.4 mm wide and 4.6 mm deep (`BRK_INSERT_D`, `BRK_INSERT_L`), which leaves 1.4 mm of skin under the recess floor.
- 4× M3×12 pan-head or socket-head screws to fix the brackets to the frame inserts. The leg and plate together are 8.8 mm thick, so about 3 mm of thread engages the insert, and the tip stays clear of the bottom of the 4.6 mm hole.
- One battery:
  - an 18650 in the cradle (a protected cell or one with a holder board), or
  - a LiPo pouch on the floor.

  The board has a BAT connector (MX1.25). Hold the battery in place with a strip of VHB or foam tape.
