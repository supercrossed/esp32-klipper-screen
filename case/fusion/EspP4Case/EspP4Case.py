"""
Desk case for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (5" 1024x600 touch display).

Run from Fusion: Utilities > Scripts and Add-Ins > "+" > Script from device > pick this folder.
The script creates a NEW design with these components:

  Front Frame   face of the case: bezel recess with a rim all round, glass pocket,
                ledge, magnet pockets. Prints face up with no supports.
  Body          wedge shell, face leaning back TILT_DEG, closed back with vents,
                USB-C side slot, button guide block, battery cradle, joint columns.
                Prints standing on its back with no supports. M3 heat-set
                inserts in the joint columns take the frame screws.
  Bezel         magnetic frame that sits flush in the recess, clamps the glass and
                shows only the active area. Covers the frame-to-body screws.
  Brackets      4 flat plates that screw the display PCB to the frame back.
  Buttons       3 printed plungers for POWER / BOOT / RESET (top edge)
  REF Display   reference glass + PCB + standoffs + component keep-out (not printed)
  REF Battery   reference 18650 (not printed)
  HW ...        heat-set inserts, screws and magnets in place (not printed)

It then exports a STEP of the whole assembly and a 3MF per printable part to
../../export (i.e. case/export in the project).

All dimensions are mm. Values marked "measure" are estimates: the Waveshare
drawing gives no thickness data, so check them against the real board and re-run.

Coordinates: world Z up, -Y is the front (viewer side), X to the right.
Face features are built in a "display frame": x right, y up the face,
z out of the face (z = 0 is the case front rim, case interior is z < 0).
"""

import adsk.core, adsk.fusion, traceback, math, os

# ============================ PARAMETERS (mm) ============================
TILT_DEG      = 20.0     # face leans back this far from vertical
WALL          = 3.0      # side / top / floor / back wall

# --- display module (from the Waveshare drawing) ---
GLASS_W, GLASS_H = 126.90, 70.70
ACTIVE_W, ACTIVE_H = 110.32, 62.28        # assumed centred on the glass
GLASS_T       = 5.0      # glass + LCD thickness                  (measure)
STACK         = 16.0     # glass front -> deepest part on back     (measure)
PCB_W, PCB_H, PCB_T = 118.50, 64.50, 1.6
PCB_OFF_LEFT  = 7.5      # front view: PCB left edge inset from glass left (measure)
GLASS_CLR     = 0.25     # pocket clearance per side
PCB_CLR       = 0.5      # opening behind the glass, clearance around the PCB
LEDGE_T       = 1.2      # ledge behind the glass border

# --- case envelope ---
SIDE_BORDER   = 10.3     # face border left/right of the glass pocket
BOTTOM_BORDER = 10.0     # face border below the pocket (chin)
TOP_BORDER    = 6.0      # face border above the pocket
DEPTH         = 53.0     # front-bottom edge to back face, horizontal (+3 after test print: bracket clearance)

# --- bezel ---
RIM           = 3.0      # case rim left around the bezel recess
BEZEL_T       = 3.0      # bezel thickness = recess depth (bezel sits flush); 2.2 mm magnet pocket + 0.8 mm skin
BEZEL_CLR     = 0.2      # gap between bezel and rim, per side
RECESS_EXTRA  = 0.5      # recess opened this much more per side (test print: bezel was tight)
BEZEL_WINDOW  = 0.5      # bezel window beyond active area, per side

# --- rounding (organic look; chosen so every part still prints without supports) ---
EDGE_R        = 6.0      # the 4 long edges: sides to top and sides to floor
FACE_R        = 2.0      # roll on the face perimeter (keep below RIM - RECESS_EXTRA)
BACK_CHAMFER  = 3.0      # back edges sit on the bed when the body prints, so 45 deg chamfer
BACK_BLEND_R  = 4.0      # fillet where the chamfer meets the walls (never steeper than 45 deg)
WINDOW_CHAMFER = 2.0     # 45 deg chamfer on the bezel window front edge, for finger access to the screen edges
WINDOW_R      = 1.0      # bezel window corner radius
GLASS_PROUD   = 0.2      # glass sits this far above the recess floor, so the bezel clamps it
NOTCH_R       = 6.0      # finger notch in the bottom rim for lifting the bezel

# --- magnets (bezel) ---
MAG_D, MAG_H  = 6.0, 2.0     # 6x2 magnets (BEZEL_T must be >= MAG_H + MAG_CLR_H + 0.8)
MAG_CLR_D     = 0.3          # pocket diameter clearance (press fit + glue)
MAG_CLR_H     = 0.2
MAG_Y         = (8.0, 74.0)  # magnet heights up the face, in the side borders

# --- front frame to body joint: M3 countersunk from the front (hidden by the bezel)
#     into M3 heat-set inserts (5 mm OD) in the body columns ---
JOINT_Y        = (26.0, 66.0) # screw heights up the face, in the side borders
JOINT_BOSS_D   = 9.4          # column diameter: 2.5 mm wall around the insert
JOINT_CLR_D    = 3.4
JOINT_CSK_D    = 6.5
JOINT_INSERT_D = 4.4          # heat-set insert hole
JOINT_INSERT_L = 10.0         # hole depth: insert length + room for the screw tip and melt

# --- USB-C ports (right side when viewed from the front) ---
USB_FROM_PCB_TOP = (23.27, 41.22)
USB_Z_BEHIND_GLASS = 3.2     # port centre behind glass back face   (measure)
USB_SLOT_H    = 10.0         # slot height (through the case thickness direction)
USB_SLOT_PAD  = 6.0          # slot extends this far past each port centre

# --- top buttons, x measured from PCB left edge in front view ---
BUTTONS = (("POWER", 27.51), ("BOOT", 39.19), ("RESET", 50.87))
BTN_SHIFT_X   = -5.0         # test-print correction; negative = towards the left edge (front view)
BTN_Z_BEHIND_GLASS = 3.4     # switch centre behind glass back face (measure)
BTN_SWITCH_PROTRUDE = 0.8    # switch actuator beyond PCB top edge   (measure)
BTN_SHAFT_D   = 5.0
BTN_HOLE_CLR  = 0.25         # radial, and on the flat
BTN_FLAT      = 1.9          # D-profile: flat this far from the axis, facing the case front; print lying on it
BTN_NUB_D     = 4.0          # nub that presses the switch (reaches the flat so it prints supported)
BTN_PROUD     = 1.5          # cap height above the top surface; caps are cut level (horizontal)

# --- display mounting holes (PCB corners) ---
# Front-view positions. The USB side is on the right. Measured from the PCB edges.
MOUNT_INSET_X_USB   = 3.25   # right-hand holes, from PCB right edge      (measure)
MOUNT_INSET_X_OTHER = 3.0    # left-hand holes, from PCB left edge        (measure)
MOUNT_INSET_Y       = 3.75   # all holes, from PCB top / bottom edge      (measure)
MOUNT_CLR_D   = 3.4          # M3 clearance; also fine for M2.5
MOUNT_DEPTH   = 12.0         # glass front to the rear face of the screw mounts (measured)
STANDOFF_D    = 5.0          # screw-mount standoff diameter, for the reference model only
BOSS_D        = 8.0          # boss, only used if the mounts sit shallower than BRK_GAP
NUT_RECESS_D  = 6.0          # optional recess in the boss face
NUT_RECESS_H  = 0.0          # 0: the plate seats flat on the standoff ends

# --- display brackets: flat plates that hold the PCB to the front frame ---
BRK_GAP       = 3.5          # minimum clearance between the plate and the PCB back
BRK_T         = 3.0
BRK_W         = 7.0
BRK_RELIEF    = 1.0          # clearance cut into the body around each bracket (test print)
BRK_INSERT_D  = 4.4          # M3 heat-set insert (5 mm OD x 4 mm long) in the frame back
BRK_INSERT_L  = 4.6          # frame is 6 mm thick here; leaves a 1.4 mm skin under the recess floor

# --- 18650 cradle ---
BATT_D        = 18.6
BATT_LEN_CUT  = 80.0
BATT_SADDLE_X = (-22.0, 22.0)

# Pick up export folder relative to this script: case/export
EXPORT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "export"))
DO_EXPORT  = True
SHOW_HARDWARE = True       # model inserts, screws and magnets (render / STEP only, never exported as 3MF)
INSERT_OD  = 5.0
# ========================================================================

app = adsk.core.Application.get()
ui = app.userInterface
tbm = adsk.fusion.TemporaryBRepManager.get()
UNION = adsk.fusion.BooleanTypes.UnionBooleanType
DIFF = adsk.fusion.BooleanTypes.DifferenceBooleanType
INTER = adsk.fusion.BooleanTypes.IntersectionBooleanType

A = math.radians(TILT_DEG)
SA, CA, TA = math.sin(A), math.cos(A), math.tan(A)


def P(x, y, z):
    return adsk.core.Point3D.create(x * 0.1, y * 0.1, z * 0.1)  # mm -> cm


def V(x, y, z):
    return adsk.core.Vector3D.create(x, y, z)


# display frame -> world
M_D = adsk.core.Matrix3D.create()
M_D.setWithCoordinateSystem(adsk.core.Point3D.create(0, 0, 0), V(1, 0, 0), V(0, SA, CA), V(0, -CA, SA))


def box(x0, x1, y0, y1, z0, z1):
    x0, x1 = sorted((x0, x1)); y0, y1 = sorted((y0, y1)); z0, z1 = sorted((z0, z1))
    obb = adsk.core.OrientedBoundingBox3D.create(
        P((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), V(1, 0, 0), V(0, 1, 0),
        (x1 - x0) * 0.1, (y1 - y0) * 0.1, (z1 - z0) * 0.1)
    return tbm.createBox(obb)


def cyl(a, b, r):
    return tbm.createCylinderOrCone(P(*a), r * 0.1, P(*b), r * 0.1)


def D(body):
    """Move a body built in the display frame into world position."""
    tbm.transform(body, M_D)
    return body


def union(target, *tools):
    for t in tools:
        tbm.booleanOperation(target, t, UNION)
    return target


def cut(target, *tools):
    for t in tools:
        tbm.booleanOperation(target, t, DIFF)
    return target


def overlap_volume(a, b):
    """Volume (mm^3) of a & b, 0 if they do not touch."""
    try:
        t = tbm.copy(a)
        if not tbm.booleanOperation(t, tbm.copy(b), INTER):
            return 0.0
        return t.volume * 1000.0 if t.faces.count else 0.0
    except:
        return 0.0


_KINDS = {"side+": (1, 0, 0), "side-": (-1, 0, 0), "top": (0, 0, 1), "floor": (0, 0, -1),
          "back": (0, 1, 0), "face": (0, -CA, SA), "+z": (0, 0, 1), "-z": (0, 0, -1)}


def _normal(face):
    ok, n = face.evaluator.getNormalAtPoint(face.pointOnFace)
    return (n.x, n.y, n.z)


def _is(face, kind):
    if face.geometry.surfaceType != adsk.core.SurfaceTypes.PlaneSurfaceType:
        return False
    n, k = _normal(face), _KINDS[kind]
    return n[0] * k[0] + n[1] * k[1] + n[2] * k[2] > 0.999


def edge_between(*pairs):
    """Selector: edge whose two faces match one of the (kindA, kindB) pairs."""
    def sel(e):
        if e.faces.count != 2:
            return False
        f0, f1 = e.faces.item(0), e.faces.item(1)
        return any((_is(f0, a) and _is(f1, b)) or (_is(f0, b) and _is(f1, a)) for a, b in pairs)
    return sel


def edge_on(kind):
    """Selector: any edge on a planar face of this kind."""
    return lambda e: any(_is(e.faces.item(i), kind) for i in range(e.faces.count))


def vertical_edges(e):
    """Selector: straight edge parallel to model Z (corners of a box seen from the top)."""
    g = e.geometry
    if g.curveType != adsk.core.Curve3DTypes.Line3DCurveType:
        return False
    d = g.startPoint.vectorTo(g.endPoint)
    return d.length > 0 and abs(d.z) / d.length > 0.999


def chamfer_to_wall_edges(e):
    """Selector: upper edge of the back chamfer, where it meets the side / top / floor walls.
    Chamfer faces are the faces that touch the planar back face."""
    def is_back(f):
        return _is(f, "back")

    def is_chamfer(f):
        if is_back(f):
            return False
        return any(is_back(g) for ed in f.edges for g in ed.faces)
    if e.faces.count != 2:
        return False
    f0, f1 = e.faces.item(0), e.faces.item(1)
    c0, c1 = is_chamfer(f0), is_chamfer(f1)
    return (c0 != c1) and not is_back(f0) and not is_back(f1)


def rounded(temp_body, steps):
    """Apply Fusion fillets / chamfers to a temporary body and return the result as a
    temporary body. steps: list of (selector, "fillet" | "chamfer", size_mm)."""
    root = adsk.fusion.Design.cast(app.activeProduct).rootComponent
    occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    comp = occ.component
    base = comp.features.baseFeatures.add()
    base.startEdit()
    comp.bRepBodies.add(temp_body, base)
    base.finishEdit()
    for sel, kind, size in steps:
        body = comp.bRepBodies.item(0)
        edges = adsk.core.ObjectCollection.create()
        for e in body.edges:
            if sel(e):
                edges.add(e)
        if edges.count == 0:
            raise RuntimeError("rounding step selected no edges (%s %.1f)" % (kind, size))
        val = adsk.core.ValueInput.createByReal(size * 0.1)
        if kind == "fillet":
            fi = comp.features.filletFeatures.createInput()
            try:
                fi.edgeSetInputs.addConstantRadiusEdgeSet(edges, val, True)
            except AttributeError:
                fi.addConstantRadiusEdgeSet(edges, val, True)
            comp.features.filletFeatures.add(fi)
        else:
            ci = comp.features.chamferFeatures.createInput2()
            ci.chamferEdgeSets.addEqualDistanceChamferEdgeSet(edges, val, True)
            comp.features.chamferFeatures.add(ci)
    result = tbm.copy(comp.bRepBodies.item(0))
    occ.deleteMe()
    return result


def build():
    if BEZEL_T < MAG_H + MAG_CLR_H + 0.8 or WINDOW_CHAMFER > BEZEL_T - 0.8:
        raise ValueError("BEZEL_T too thin for MAG_H / WINDOW_CHAMFER")
    # ---------------- derived geometry ----------------
    W = GLASS_W + 2 * GLASS_CLR + 2 * SIDE_BORDER
    H = BOTTOM_BORDER + 2 * GLASS_CLR + GLASS_H + TOP_BORDER    # face height along the slope
    BZ = H * CA                                                  # case height

    pk_x = GLASS_W / 2 + GLASS_CLR             # glass pocket half width
    pk_y0 = BOTTOM_BORDER
    pk_y1 = BOTTOM_BORDER + GLASS_H + 2 * GLASS_CLR
    g_y0, g_y1 = pk_y0 + GLASS_CLR, pk_y1 - GLASS_CLR
    g_cy = (g_y0 + g_y1) / 2

    z_r = -BEZEL_T                             # recess floor
    z_gf = z_r + GLASS_PROUD                   # glass front
    z_gb = z_gf - GLASS_T                      # glass back
    z_split = z_gb - LEDGE_T                   # back of the front frame = joint with the body
    z_pb = z_gb - PCB_T                        # PCB back face
    z_mt = z_gf - MOUNT_DEPTH                  # rear face of the PCB screw mounts
    z_bf = min(z_pb - BRK_GAP, z_mt)           # bracket plate front face: on the mounts, or deeper

    px0 = -GLASS_W / 2 + PCB_OFF_LEFT
    px1 = px0 + PCB_W
    py0 = g_y0 + (GLASS_H - PCB_H) / 2
    py1 = py0 + PCB_H

    rx = W / 2 - RIM                           # recess / bezel outline
    ry0, ry1 = RIM, H - RIM
    side_x = (pk_x + rx) / 2                   # centre line of the side borders

    # ---------------- envelope ----------------
    env0 = box(-W / 2, W / 2, 0, DEPTH, 0, BZ)
    cut(env0, D(box(-500, 500, -500, 500, 0, 500)))             # remove everything in front of the face
    env0 = rounded(env0, [
        (edge_between(("side+", "top"), ("side-", "top"), ("side+", "floor"), ("side-", "floor")), "fillet", EDGE_R),
        (edge_on("face"), "fillet", FACE_R),
        (edge_on("back"), "chamfer", BACK_CHAMFER),
        (chamfer_to_wall_edges, "fillet", BACK_BLEND_R),
    ])

    def envelope():
        return tbm.copy(env0)

    def front_half():
        return D(box(-500, 500, -500, 500, z_split, 500))

    # ================= FRONT FRAME =================
    frame = envelope()
    tbm.booleanOperation(frame, front_half(), INTER)

    # ================= BODY (closed back, open front) =================
    body_env = envelope()
    cut(body_env, front_half())
    body = tbm.copy(body_env)
    cavity = rounded(box(-W / 2 + WALL, W / 2 - WALL, -50, DEPTH - WALL, WALL, BZ - WALL), [
        (edge_between(("side+", "top"), ("side-", "top"), ("side+", "floor"), ("side-", "floor")), "fillet", EDGE_R - WALL),
    ])
    cut(body, cavity)

    # frame-to-body joint: full-length columns from the joint back to the back wall
    joints = [(sx * side_x, y) for sx in (-1, 1) for y in JOINT_Y]
    for (x, y) in joints:
        z_end = -(DEPTH - y * SA) / CA - 2
        union(body, D(cyl((x, y, z_split), (x, y, z_end), JOINT_BOSS_D / 2)))

    # button guide block: solid from the top wall down to below the button axis, back to the back wall
    btn_z = z_gb - BTN_Z_BEHIND_GLASS
    ys = py1 + BTN_SWITCH_PROTRUDE                 # top of switch actuator
    y_top_axis = H - btn_z * TA                    # where the axis meets the top surface
    hole_r = BTN_SHAFT_D / 2 + BTN_HOLE_CLR
    flange_r = hole_r + 0.45
    btn_x = [(name, px0 + d + BTN_SHIFT_X) for name, d in BUTTONS]
    bx = [x for _, x in btn_x]
    y_s0 = ys + 2.6                                # bottom of the guide along the axis
    z_low = y_s0 * CA + (btn_z - hole_r - 1.5) * SA
    union(body, box(min(bx) - 6, max(bx) + 6, -10, DEPTH - WALL + 0.1, z_low, BZ - WALL + 0.1))

    # 18650 cradle saddles, run back to the back wall so they print without support
    batt_r = BATT_D / 2
    b_y = DEPTH - WALL - 0.5 - batt_r
    b_z = WALL + batt_r + 0.3
    for x in BATT_SADDLE_X:
        # front face stops where the cradle arc reaches 45 deg, so the arc never overhangs
        union(body, box(x - 3, x + 3, b_y - 0.7 * batt_r, DEPTH - WALL + 0.1, WALL - 0.1, b_z))

    tbm.booleanOperation(body, body_env, INTER)    # trim to the body envelope

    # ---------------- frame cuts ----------------
    recess_r = EDGE_R - RIM                                                         # follows the outer corner radius
    E = RECESS_EXTRA                                                                # extra bezel clearance in the frame only
    cut(frame, D(rounded(box(-rx - E, rx + E, ry0 - E, ry1 + E, 1, z_r), [(vertical_edges, "fillet", recess_r + E)])))  # bezel recess
    cut(frame, D(box(-pk_x, pk_x, pk_y0, pk_y1, z_r + 0.1, z_gb)))                  # glass pocket
    cut(frame, D(box(px0 - PCB_CLR, min(px1 + PCB_CLR, pk_x - 0.4),                 # opening behind the glass
                     py0 - PCB_CLR, py1 + PCB_CLR, z_gb + 0.1, z_split - 1)))
    cut(frame, D(box(min(bx) - 5, max(bx) + 5, py1, pk_y1, z_gb + 0.1, z_split - 1)))  # switch clearance in the ledge
    cut(frame, D(cyl((0, 0, 1), (0, 0, z_r + 1.5), NOTCH_R)))                       # finger notch

    mag_r = MAG_D / 2 + MAG_CLR_D / 2
    mag_depth = MAG_H + MAG_CLR_H
    mag_pts = [(sx * side_x, y) for sx in (-1, 1) for y in MAG_Y]
    for (x, y) in mag_pts:
        cut(frame, D(cyl((x, y, z_r + 0.1), (x, y, z_r - mag_depth), mag_r)))

    for (x, y) in joints:                                                           # countersunk M3 from the front
        cut(frame, D(cyl((x, y, z_r + 1), (x, y, z_split - 1), JOINT_CLR_D / 2)))
        cut(frame, D(tbm.createCylinderOrCone(P(x, y, z_r + 0.01), JOINT_CSK_D / 2 * 0.1,
                                              P(x, y, z_r - (JOINT_CSK_D - JOINT_CLR_D) / 2), JOINT_CLR_D / 2 * 0.1)))
        cut(body, D(cyl((x, y, z_split + 1), (x, y, z_split - JOINT_INSERT_L), JOINT_INSERT_D / 2)))  # heat-set insert

    # ---------------- display brackets ----------------
    holes = {"TR": (px1 - MOUNT_INSET_X_USB, py1 - MOUNT_INSET_Y), "BR": (px1 - MOUNT_INSET_X_USB, py0 + MOUNT_INSET_Y),
             "TL": (px0 + MOUNT_INSET_X_OTHER, py1 - MOUNT_INSET_Y), "BL": (px0 + MOUNT_INSET_X_OTHER, py0 + MOUNT_INSET_Y)}
    legs = {"TR": (holes["TR"][0], py1 + PCB_CLR + BRK_W / 2 + 2.75),
            "TL": (holes["TL"][0], py1 + PCB_CLR + BRK_W / 2 + 2.75),
            "BR": (side_x, holes["BR"][1]),
            "BL": (-side_x, holes["BL"][1])}
    brackets, bracket_solids = [], []
    hw = BRK_W / 2
    for key in ("TL", "TR", "BL", "BR"):
        (hx, hy), (lx, ly) = holes[key], legs[key]
        # top legs sit under the body's top wall: trim their top edge to 0.9 mm past the screw hole
        top = MOUNT_CLR_D / 2 + 0.9 if key in ("TL", "TR") else hw
        b = box(min(hx, lx) - hw, max(hx, lx) + hw, min(hy, ly) - hw, max(hy, ly) + top, z_bf, z_bf - BRK_T)  # plate
        if z_mt - z_bf > 0.05:                                                                             # boss up to the mounts
            union(b, cyl((hx, hy, z_mt), (hx, hy, z_bf - 0.1), BOSS_D / 2))
        union(b, box(lx - hw, lx + hw, ly - hw, ly + top, z_split, z_bf - 0.1))                             # leg to the frame
        cut(b, cyl((hx, hy, z_mt + 1), (hx, hy, z_bf - BRK_T - 1), MOUNT_CLR_D / 2))
        cut(b, cyl((lx, ly, z_split + 1), (lx, ly, z_bf - BRK_T - 1), MOUNT_CLR_D / 2))
        if NUT_RECESS_H > 0:
            cut(b, cyl((hx, hy, z_mt + 0.1), (hx, hy, z_mt - NUT_RECESS_H), NUT_RECESS_D / 2))
        cut(frame, D(cyl((lx, ly, z_split - 1), (lx, ly, z_split + BRK_INSERT_L), BRK_INSERT_D / 2)))  # heat-set insert
        # relief in the body around the bracket, so the body slides over it with clearance all round
        m = BRK_RELIEF
        cut(body, D(box(min(hx, lx) - hw - m, max(hx, lx) + hw + m, min(hy, ly) - hw - m, max(hy, ly) + top + m,
                        z_split + 1, z_bf - BRK_T - m)))
        D(b)
        bracket_solids.append(tbm.copy(b))
        brackets.append(("Bracket_" + key, b))

    # ---------------- body cuts ----------------
    for _, x in btn_x:                                                              # button holes + flange travel
        for part in (body, frame):
            dh = cyl((x, ys + 2.0, btn_z), (x, y_top_axis + 20, btn_z), hole_r)           # D-shaped hole
            tbm.booleanOperation(dh, box(x - 10, x + 10, ys - 5, y_top_axis + 30,
                                         btn_z - 10, btn_z + BTN_FLAT + BTN_HOLE_CLR), INTER)
            cut(part, D(dh))
            cut(part, D(cyl((x, ys - 1.0, btn_z), (x, y_s0, btn_z), flange_r + 0.4)))
    usb_y = [py1 - d for d in USB_FROM_PCB_TOP]                                     # USB-C slot, right side
    usb_z = z_gb - USB_Z_BEHIND_GLASS
    usb_cut = box(pk_x, W / 2 + 2, min(usb_y) - USB_SLOT_PAD, max(usb_y) + USB_SLOT_PAD,
                  usb_z - USB_SLOT_H / 2, usb_z + USB_SLOT_H / 2)
    D(usb_cut)
    cut(body, tbm.copy(usb_cut))
    cut(frame, usb_cut)
    cut(body, cyl((-BATT_LEN_CUT / 2, b_y, b_z), (BATT_LEN_CUT / 2, b_y, b_z), batt_r))  # battery cradle
    for i in range(6):                                                              # vents in the back wall
        z = BZ * 0.45 + i * 5.0
        if z + 1.25 < BZ - WALL - 12:
            cut(body, box(-35, 35, DEPTH - WALL - 1, DEPTH + 1, z - 1.25, z + 1.25))

    # ---------------- bezel ----------------
    bezel = rounded(box(-rx + BEZEL_CLR, rx - BEZEL_CLR, ry0 + BEZEL_CLR, ry1 - BEZEL_CLR, z_r, 0),
                    [(vertical_edges, "fillet", recess_r - BEZEL_CLR)])
    cut(bezel, rounded(box(-ACTIVE_W / 2 - BEZEL_WINDOW, ACTIVE_W / 2 + BEZEL_WINDOW,
                           g_cy - ACTIVE_H / 2 - BEZEL_WINDOW, g_cy + ACTIVE_H / 2 + BEZEL_WINDOW, z_r - 1, 1),
                       [(vertical_edges, "fillet", WINDOW_R)]))
    win_hw = ACTIVE_W / 2 + BEZEL_WINDOW + 1.0
    win_hh = ACTIVE_H / 2 + BEZEL_WINDOW + 1.0

    def window_front_edge(e):
        bb = e.boundingBox
        inside = (max(abs(bb.minPoint.x), abs(bb.maxPoint.x)) * 10 < win_hw and
                  max(abs(bb.minPoint.y * 10 - g_cy), abs(bb.maxPoint.y * 10 - g_cy)) < win_hh)
        return inside and edge_on("+z")(e)
    bezel = rounded(bezel, [(window_front_edge, "chamfer", WINDOW_CHAMFER)])       # chamfered window edge only
    for (x, y) in mag_pts:
        cut(bezel, cyl((x, y, z_r - 0.1), (x, y, z_r + mag_depth), mag_r))
    D(bezel)

    # ---------------- buttons ----------------
    buttons = []
    for name, x in btn_x:
        b = cyl((x, ys + 0.2, btn_z), (x, ys + 0.9, btn_z), BTN_NUB_D / 2)          # nub on the switch
        union(b, cyl((x, ys + 0.9, btn_z), (x, ys + 2.0, btn_z), flange_r))          # retaining flange
        union(b, cyl((x, ys + 2.0, btn_z), (x, y_top_axis + BTN_PROUD / CA + 5, btn_z), BTN_SHAFT_D / 2))  # long shaft
        cut(b, box(x - 10, x + 10, ys - 5, y_top_axis + 20, btn_z + BTN_FLAT, btn_z + 10))  # D flat
        D(b)
        cut(b, box(-500, 500, -500, 500, BZ + BTN_PROUD, BZ + 100))                   # level top, parallel to the case top
        buttons.append(("Button_" + name, b))

    # ---------------- reference display ----------------
    glass = D(box(-GLASS_W / 2, GLASS_W / 2, g_y0, g_y1, z_gf, z_gb))
    pcb = box(px0, px1, py0, py1, z_gb, z_pb)
    for (x, y) in holes.values():
        cut(pcb, cyl((x, y, z_gb + 1), (x, y, z_pb - 1), 1.35))
        so = cyl((x, y, z_pb + 0.01), (x, y, z_mt), STANDOFF_D / 2)               # screw-mount standoffs
        cut(so, cyl((x, y, z_pb + 1), (x, y, z_mt - 1), 1.35))
        union(pcb, so)
    D(pcb)
    keepout = D(box(px0, px1, py0, py1, z_pb, z_gf - STACK))
    for s in bracket_solids:          # the brackets are meant to sit over the PCB corners
        cut(keepout, s)
    battery = cyl((-34, b_y, b_z), (34, b_y, b_z), 9.25)

    # ---------------- interference report ----------------
    report = []
    printables = [("Front Frame", frame), ("Body", body)] + buttons + brackets
    for rname, ref in (("glass", glass), ("PCB", pcb), ("component keep-out", keepout), ("18650", battery)):
        for pname, part in printables:
            v = overlap_volume(part, ref)
            if v > 0.5:
                report.append("%s overlaps %s by %.0f mm^3" % (pname, rname, v))
    for pname, part in [("Body", body)] + brackets + buttons:
        for qname, other in [("Front Frame", frame)] + ([("Body", body)] if pname != "Body" else []):
            v = overlap_volume(part, other)
            if v > 0.5:
                report.append("%s overlaps %s by %.0f mm^3" % (pname, qname, v))
    v = overlap_volume(battery, keepout)
    if v > 0.5:
        report.append("18650 overlaps component keep-out by %.0f mm^3 (increase DEPTH)" % v)

    # ---------------- hardware (for renders / STEP only, never printed) ----------------
    # All built in the display frame along the screw axis (z), then moved into place.
    def hexagon(x, y, z0, z1, af):
        """Hex prism (across flats af) along z, for socket recesses."""
        h = None
        for k in range(3):
            ang = math.radians(60 * k)
            obb = adsk.core.OrientedBoundingBox3D.create(
                P(x, y, (z0 + z1) / 2), V(math.cos(ang), math.sin(ang), 0), V(-math.sin(ang), math.cos(ang), 0),
                af * 2 * 0.1, af * 0.1, abs(z1 - z0) * 0.1)
            b = tbm.createBox(obb)
            if h is None:
                h = b
            else:
                tbm.booleanOperation(h, b, INTER)
        return h

    def heat_insert(x, y, z_flush, length, inward):
        """Knurled brass insert, flush face at z_flush, body running `inward` (+1 / -1 along z)."""
        z_end = z_flush + inward * length
        b = cyl((x, y, z_flush), (x, y, z_end), INSERT_OD / 2)
        for t in (0.3, 0.65):                                   # two knurl grooves
            zc = z_flush + inward * length * t
            ring = cyl((x, y, zc - 0.35), (x, y, zc + 0.35), INSERT_OD / 2 + 0.1)
            cut(ring, cyl((x, y, zc - 1), (x, y, zc + 1), INSERT_OD / 2 - 0.3))
            cut(b, ring)
        cut(b, cyl((x, y, z_flush - 1), (x, y, z_end + 1), 1.25))  # M3 thread bore
        return D(b)

    def socket_screw(x, y, z_seat, length, inward):
        """M3 socket-head cap screw: head sits on z_seat, shaft runs `inward`."""
        head_top = z_seat - inward * 3.0
        b = cyl((x, y, z_seat), (x, y, head_top), 2.75)
        union(b, cyl((x, y, z_seat), (x, y, z_seat + inward * length), 1.5))
        cut(b, hexagon(x, y, head_top - inward * 1, head_top + inward * 1.6, 2.5))
        return D(b)

    def countersunk_screw(x, y, z_top, length, inward):
        """M3 countersunk screw: head top flush at z_top, shaft runs `inward`."""
        b = tbm.createCylinderOrCone(P(x, y, z_top), 3.0 * 0.1, P(x, y, z_top + inward * 1.65), 1.5 * 0.1)
        union(b, cyl((x, y, z_top + inward * 1.6), (x, y, z_top + inward * length), 1.5))
        cut(b, hexagon(x, y, z_top - inward * 1, z_top + inward * 1.3, 2.0))
        return D(b)

    hw_body_inserts = [("Insert_body_%d" % i, heat_insert(x, y, z_split, 5.7, -1)) for i, (x, y) in enumerate(joints)]
    hw_frame_inserts = [("Insert_frame_" + k, heat_insert(legs[k][0], legs[k][1], z_split, 4.0, +1))
                        for k in ("TL", "TR", "BL", "BR")]
    hw_joint_screws = [("M3x12_countersunk_%d" % i, countersunk_screw(x, y, z_r, 12.0, -1)) for i, (x, y) in enumerate(joints)]
    z_back = z_bf - BRK_T
    hw_bracket_screws = [("M3x12_socket_" + k, socket_screw(legs[k][0], legs[k][1], z_back, 12.0, +1))
                         for k in ("TL", "TR", "BL", "BR")]
    hw_pcb_screws = [("M3x6_socket_" + k, socket_screw(holes[k][0], holes[k][1], z_back, 6.0, +1))
                     for k in ("TL", "TR", "BL", "BR")]
    hw_frame_magnets = [("Magnet_frame_%d" % i, D(cyl((x, y, z_r - mag_depth), (x, y, z_r - mag_depth + MAG_H), MAG_D / 2)))
                        for i, (x, y) in enumerate(mag_pts)]
    hw_bezel_magnets = [("Magnet_bezel_%d" % i, D(cyl((x, y, z_r + mag_depth - MAG_H), (x, y, z_r + mag_depth), MAG_D / 2)))
                        for i, (x, y) in enumerate(mag_pts)]

    info = {"W": W, "H": H, "BZ": BZ, "DEPTH": DEPTH}
    parts = [
        ("Front Frame", [("Front Frame", frame)]),
        ("Body", [("Body", body)]),
        ("Bezel", [("Bezel", bezel)]),
        ("Brackets", brackets),
        ("Buttons", buttons),
        ("REF Display", [("REF Glass", glass), ("REF PCB", pcb), ("REF Keepout", keepout)]),
        ("REF Battery", [("REF 18650", battery)]),
    ]
    if SHOW_HARDWARE:
        parts += [
            ("HW Body Inserts", hw_body_inserts),
            ("HW Frame Inserts", hw_frame_inserts),
            ("HW Joint Screws", hw_joint_screws),
            ("HW Bracket Screws", hw_bracket_screws),
            ("HW PCB Screws", hw_pcb_screws),
            ("HW Frame Magnets", hw_frame_magnets),
            ("HW Bezel Magnets", hw_bezel_magnets),
        ]
    return parts, report, info


def y_up_fix():
    """Return a matrix to convert Z-up geometry to Y-up if Fusion is set to Y-up, else None."""
    try:
        o = app.preferences.generalPreferences.defaultModelingOrientation
        if o == adsk.core.DefaultModelingOrientations.YUpModelingOrientation:
            m = adsk.core.Matrix3D.create()
            m.setWithCoordinateSystem(adsk.core.Point3D.create(0, 0, 0), V(1, 0, 0), V(0, 0, -1), V(0, 1, 0))
            return m
    except:
        pass
    return None


def run(context):
    try:
        ui.messageBox(main(), "ESP32-P4 case")
    except:
        if ui:
            ui.messageBox("Failed:\n{}".format(traceback.format_exc()))


def main():
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.ParametricDesignType
    root = design.rootComponent
    parts, report, info = build()      # uses temporary components in this design for the fillets
    fix = y_up_fix()

    occs = {}
    for comp_name, bodies in parts:
        occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        comp = occ.component
        comp.name = comp_name
        base = comp.features.baseFeatures.add()
        base.startEdit()
        for bname, b in bodies:
            if fix:
                tbm.transform(b, fix)
            nb = comp.bRepBodies.add(b, base)
            nb.name = bname
        base.finishEdit()
        if comp_name.startswith("REF"):
            for b in comp.bRepBodies:
                if b.name == "REF Keepout":     # clearance envelope: hidden, but kept for checks
                    b.isLightBulbOn = False
                    try:
                        b.opacity = 0.35
                    except:
                        pass
        occs[comp_name] = occ

    app.activeViewport.fit()

    exported = []
    if DO_EXPORT:
        os.makedirs(EXPORT_DIR, exist_ok=True)
        em = design.exportManager
        step_path = os.path.join(EXPORT_DIR, "esp32-p4-case-assembly.step")
        em.execute(em.createSTEPExportOptions(step_path, root))
        exported.append(os.path.basename(step_path))
        for comp_name, occ in occs.items():
            if comp_name.startswith(("REF", "HW")):
                continue
            fname = "esp32-p4-case-" + comp_name.lower().replace(" ", "-") + ".3mf"
            try:
                em.execute(em.createC3MFExportOptions(occ, os.path.join(EXPORT_DIR, fname)))
                exported.append(fname)
            except:
                # older builds: fall back to STL
                sname = fname[:-4] + ".stl"
                opt = em.createSTLExportOptions(occ, os.path.join(EXPORT_DIR, sname))
                opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
                em.execute(opt)
                exported.append(sname)

    msg = "Case %.1f W x %.1f D x %.1f H mm, face %.1f mm along slope.\n\n" % (
        info["W"], info["DEPTH"], info["BZ"], info["H"])
    msg += ("Interference check:\n  " + "\n  ".join(report)) if report else "Interference check: clear."
    if exported:
        msg += "\n\nExported to %s:\n  %s" % (EXPORT_DIR, "\n  ".join(exported))
    return msg
