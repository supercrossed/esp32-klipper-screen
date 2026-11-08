"""
Render the build-guide screenshots for the ESP32-P4 display case.

Run EspP4Case.py first (it builds the design, including the HW components), keep that
design active, then run this. It colours the parts, stages each assembly step (exploded
along the screw axes) and saves PNGs to case/export as build-NN-*.png. Afterwards every
part is put back in place.

From Fusion: load this file with the Scripts and Add-Ins dialog the same way as the
generator, or exec() it and call main().
"""

import adsk.core, adsk.fusion, math, os, traceback

TILT_DEG = 20.0
IMG_W, IMG_H = 1600, 1000
MARGIN = 1.12      # >1 zooms out after fitting, so nothing is cropped
ACTIVE_W, ACTIVE_H = 110.32, 62.28   # display active area, centred on the glass
SCREEN_UI = {}     # shot name -> UI screenshot; filled in main(), composited by overlay_screens.py
OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "export"))
QUAD_DIR = os.path.join(OUT_DIR, "_render")   # screen quads + masks for overlay_screens.py; None = skip

COLOURS = {                       # occurrence -> appearance from the Fusion Appearance Library
    "Front Frame": "Plastic - Matte (Gray)",
    "Body": "Plastic - Matte (Gray)",
    "Bezel": "Plastic - Matte (Black)",
    "Brackets": "Plastic - Matte (Yellow)",
    "Buttons": "Plastic - Matte (Red)",
    "REF Battery": "Plastic - Glossy (Green)",
    "HW Body Inserts": "Brass - Polished",
    "HW Frame Inserts": "Brass - Polished",
    "HW Joint Screws": "Stainless Steel - Satin",
    "HW Bracket Screws": "Coating - Black Oxide",
    "HW PCB Screws": "Coating - Black Oxide",
    "HW Frame Magnets": "Nickel - Polished",
    "HW Bezel Magnets": "Nickel - Polished",
}
BODY_COLOURS = {"REF Glass": "Paint - Enamel Glossy (Black)", "REF PCB": "Plastic - Matte (Blue)"}

app = adsk.core.Application.get()
A = math.radians(TILT_DEG)
S, C = math.sin(A), math.cos(A)


def _yup():
    try:
        return (app.preferences.generalPreferences.defaultModelingOrientation ==
                adsk.core.DefaultModelingOrientations.YUpModelingOrientation)
    except:
        return False


YUP = _yup()


def W(v):
    """Z-up model vector/point (mm) -> Fusion world (mm)."""
    x, y, z = v
    return (x, z, -y) if YUP else (x, y, z)


def add(*vs):
    return tuple(sum(c) for c in zip(*vs))


def mul(v, k):
    return tuple(c * k for c in v)


def norm(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v)


# directions in the Z-up model
F = (0, -C, S)          # out of the screen, towards the viewer
U = (0, S, C)           # up the face of the screen
X = (1, 0, 0)
Z = (0, 0, 1)


class Scene:
    def __init__(self):
        self.design = adsk.fusion.Design.cast(app.activeProduct)
        self.root = self.design.rootComponent
        self.occ = {o.component.name: o for o in self.root.occurrences}
        missing = [n for n in COLOURS if n not in self.occ]
        if missing:
            raise RuntimeError("Run EspP4Case.py first (with SHOW_HARDWARE = True). Missing: %s" % missing)
        self.vp = app.activeViewport

    # ---------- look ----------
    def appearance(self, name):
        a = self.design.appearances.itemByName(name)
        if a:
            return a
        lib = app.materialLibraries.itemByName("Fusion Appearance Library")
        return self.design.appearances.addByCopy(lib.appearances.itemByName(name), name)

    def colour(self):
        for n, a in COLOURS.items():
            self.occ[n].appearance = self.appearance(a)
        for b in self.occ["REF Display"].component.bRepBodies:
            if b.name in BODY_COLOURS:
                b.appearance = self.appearance(BODY_COLOURS[b.name])
            if b.name == "REF Keepout":
                b.isLightBulbOn = False
            try:
                b.opacity = 1.0 if b.name != "REF Keepout" else 0.35
            except:
                pass
        try:
            self.root.isOriginFolderLightBulbOn = False
        except:
            pass
        try:
            self.vp.visualStyle = adsk.core.VisualStyles.ShadedWithVisibleEdgesOnlyVisualStyle
        except:
            pass

    # ---------- staging ----------
    def stage(self, show, moves=None):
        """show: names to make visible. moves: {name: Z-up offset vector in mm}."""
        moves = moves or {}
        self._last = (show, moves)
        for n, o in self.occ.items():
            o.isLightBulbOn = n in show
            off = W(moves.get(n, (0, 0, 0)))
            m = adsk.core.Matrix3D.create()
            m.translation = adsk.core.Vector3D.create(off[0] / 10, off[1] / 10, off[2] / 10)
            o.transform2 = m

    def reset(self):
        self.stage(list(self.occ.keys()))
        for b in self.occ["REF Display"].component.bRepBodies:
            if b.name == "REF Keepout":
                b.isLightBulbOn = False

    # ---------- camera ----------
    def look(self, direction, target=(0, 25, 40), zoom=1.0):
        """direction: Z-up vector from target towards the eye."""
        d = norm(direction)
        t = W(target)
        e = W(add(target, mul(d, 600)))
        cam = self.vp.camera
        cam.isSmoothTransition = False
        cam.cameraType = adsk.core.CameraTypes.PerspectiveCameraType
        cam.target = adsk.core.Point3D.create(t[0] / 10, t[1] / 10, t[2] / 10)
        cam.eye = adsk.core.Point3D.create(e[0] / 10, e[1] / 10, e[2] / 10)
        up = W(Z)
        cam.upVector = adsk.core.Vector3D.create(*up)
        cam.isFitView = True
        self.vp.camera = cam
        self.vp.fit()
        # the saved image can be a different shape from the viewport, so leave margin (MARGIN)
        cam = self.vp.camera
        cam.isSmoothTransition = False
        cam.isFitView = False
        k = MARGIN / zoom                       # perspective: zoom by moving the eye along the view line
        t, e = cam.target, cam.eye
        cam.eye = adsk.core.Point3D.create(t.x + (e.x - t.x) * k, t.y + (e.y - t.y) * k, t.z + (e.z - t.z) * k)
        self.vp.camera = cam
        self.vp.refresh()

    def look_at(self, target, direction, extent_mm):
        """Fixed camera: no auto-fit. extent_mm is roughly the width of the view."""
        d = norm(direction)
        t = W(target)
        e = W(add(target, mul(d, 300)))
        cam = self.vp.camera
        cam.isSmoothTransition = False
        cam.isFitView = False
        cam.cameraType = adsk.core.CameraTypes.PerspectiveCameraType
        cam.target = adsk.core.Point3D.create(t[0] / 10, t[1] / 10, t[2] / 10)
        cam.eye = adsk.core.Point3D.create(e[0] / 10, e[1] / 10, e[2] / 10)
        cam.upVector = adsk.core.Vector3D.create(*W(Z))
        cam.viewExtents = extent_mm / 10
        self.vp.camera = cam
        self.vp.refresh()

    def size(self):
        """Image size with the viewport's own aspect ratio, so pixels map 1:1 to the view."""
        return IMG_W, int(round(IMG_W * self.vp.height / float(self.vp.width)))

    def shot(self, name):
        path = os.path.join(OUT_DIR, name)
        adsk.doEvents()
        self.vp.refresh()
        w, h = self.size()
        if not self.vp.saveAsImageFile(path, w, h):
            raise RuntimeError("could not save " + path)
        return name

    def screen_quad(self):
        """Image-pixel corners (TL, TR, BR, BL) of the display active area in the current view."""
        o = self.occ["REF Display"]
        glass = [b for b in o.component.bRepBodies if b.name == "REF Glass"][0].createForAssemblyContext(o)
        f, u, x = W(F), W(U), W(X)
        pts = [v.geometry for v in glass.vertices]
        dot = lambda p, d: p.x * d[0] + p.y * d[1] + p.z * d[2]
        front = sorted(pts, key=lambda p: -dot(p, f))[:4]
        c = [sum(p.x for p in front) / 4, sum(p.y for p in front) / 4, sum(p.z for p in front) / 4]
        w, h = self.size()
        # modelToViewSpace works in physical pixels (display scaling); the camera target sits at
        # the centre of the view, which gives the true view width.
        ctr = self.vp.modelToViewSpace(self.vp.camera.target)
        k = w / (2.0 * ctr.x)
        quad = []
        for sx, sy in ((-1, 1), (1, 1), (1, -1), (-1, -1)):
            q = [c[i] + (sx * ACTIVE_W / 2 * x[i] + sy * ACTIVE_H / 2 * u[i]) / 10 for i in range(3)]
            p2 = self.vp.modelToViewSpace(adsk.core.Point3D.create(*q))
            quad.append((p2.x * k, p2.y * k))
        return quad

    def screen_shot(self, name, ui_image):
        """Normal shot, plus (if QUAD_DIR is set) the screen quad and a visibility mask for compositing."""
        self.shot(name)
        if not QUAD_DIR:
            return name
        o = self.occ["REF Display"]
        glass = [b for b in o.component.bRepBodies if b.name == "REF Glass"][0]
        old = glass.appearance
        glass.appearance = self.appearance("Plastic - Matte (Green)")    # flat key colour for the mask pass
        try:
            self.vp.visualStyle = adsk.core.VisualStyles.ShadedVisualStyle
        except:
            pass
        self.stage(*self._last)          # the appearance change recomputes and drops exploded positions
        adsk.doEvents()
        w, h = self.size()
        self.vp.saveAsImageFile(os.path.join(QUAD_DIR, name.replace(".png", "_mask.png")), w, h)
        SCREEN_UI[name] = {"ui": ui_image, "quad": self.screen_quad()}
        glass.appearance = old
        try:
            self.vp.visualStyle = adsk.core.VisualStyles.ShadedWithVisibleEdgesOnlyVisualStyle
        except:
            pass
        self.stage(*self._last)
        return name


PRINTED = ["Front Frame", "Body", "Bezel", "Brackets", "Buttons"]
DISPLAY = ["REF Display"]
ALL = list(COLOURS.keys()) + DISPLAY

FRONT_RIGHT = add(F, mul(X, 0.75), mul(Z, 0.35))
FRONT_LEFT = add(F, mul(X, -0.75), mul(Z, 0.35))
BACK_LEFT = add(mul(F, -1), mul(X, -0.8), mul(Z, 0.55))
BACK_RIGHT = add(mul(F, -1), mul(X, 0.7), mul(Z, 0.3))
INTO_BODY = add(mul(F, 1.0), mul(X, 0.35), mul(Z, 0.55))   # looking into the open front of the body
TOP_FRONT = add(mul(F, 0.5), mul(X, -0.25), mul(Z, 1.0))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if QUAD_DIR:
        os.makedirs(QUAD_DIR, exist_ok=True)
    sc = Scene()
    sc.colour()
    out = []
    frame_asm = ["Front Frame", "REF Display", "Brackets", "HW Frame Inserts", "HW Bracket Screws",
                 "HW PCB Screws", "HW Frame Magnets"]
    body_asm = ["Body", "HW Body Inserts", "Buttons", "REF Battery"]
    try:
        # 01 / 02 finished case
        sc.stage(ALL)
        sc.look(FRONT_RIGHT, zoom=1.05)
        out.append(sc.screen_shot("build-01-finished-front.png", "home.png"))
        sc.look(BACK_LEFT, zoom=0.92)
        out.append(sc.shot("build-02-finished-back.png"))

        # 03 printed parts, exploded
        sc.stage(PRINTED, {
            "Bezel": mul(F, 120),
            "Front Frame": mul(F, 60),
            "Brackets": mul(F, 30),
            "Buttons": mul(Z, 35),
        })
        sc.look(FRONT_RIGHT, target=(0, 0, 45))
        out.append(sc.shot("build-03-printed-parts.png"))

        # 04 heat-set inserts into the body columns
        sc.stage(["Body", "HW Body Inserts"], {"HW Body Inserts": mul(F, 18)})
        sc.look(INTO_BODY, zoom=1.1)
        out.append(sc.shot("build-04-body-inserts.png"))

        # 05 heat-set inserts into the back of the front frame
        sc.stage(["Front Frame", "HW Frame Inserts"], {"HW Frame Inserts": mul(F, -14)})
        sc.look(BACK_RIGHT, zoom=0.82)
        out.append(sc.shot("build-05-frame-inserts.png"))

        # 06 magnets into the frame recess
        sc.stage(["Front Frame", "HW Frame Magnets", "HW Frame Inserts"], {"HW Frame Magnets": mul(F, 14)})
        sc.look(FRONT_LEFT, zoom=1.1)
        out.append(sc.shot("build-06-frame-magnets.png"))

        # 07 magnets into the back of the bezel
        sc.stage(["Bezel", "HW Bezel Magnets"], {"HW Bezel Magnets": mul(F, -12)})
        sc.look(BACK_LEFT, zoom=0.92)
        out.append(sc.shot("build-07-bezel-magnets.png"))

        # 08 display into the frame pocket (glass first), seen from behind
        sc.stage(["Front Frame", "HW Frame Inserts", "HW Frame Magnets", "REF Display"],
                 {"REF Display": mul(F, -35)})
        sc.look(BACK_RIGHT, zoom=0.8)
        out.append(sc.shot("build-08-display-into-frame.png"))

        # 09 brackets and screws, exploded
        sc.stage(frame_asm, {
            "Brackets": mul(F, -14),
            "HW PCB Screws": mul(F, -32),
            "HW Bracket Screws": mul(F, -32),
        })
        sc.look(BACK_RIGHT, zoom=0.8)
        out.append(sc.shot("build-09-brackets.png"))

        # 10 frame assembly finished
        sc.stage(frame_asm)
        sc.look(BACK_RIGHT, zoom=0.82)
        out.append(sc.shot("build-10-frame-assembly.png"))

        # 11 buttons and battery into the body
        sc.stage(body_asm, {"Buttons": add(mul(F, 25), mul(Z, -8)), "REF Battery": mul(F, 45)})
        sc.look(INTO_BODY, zoom=1.0)
        out.append(sc.shot("build-11-buttons-battery.png"))

        # 12 join the frame to the body
        sc.stage(body_asm + frame_asm + ["HW Joint Screws"], {
            "Front Frame": mul(F, 45), "REF Display": mul(F, 45), "Brackets": mul(F, 45),
            "HW Frame Inserts": mul(F, 45), "HW Bracket Screws": mul(F, 45), "HW PCB Screws": mul(F, 45),
            "HW Frame Magnets": mul(F, 45), "HW Joint Screws": mul(F, 75),
        })
        sc.look(FRONT_RIGHT, target=(0, 0, 45))
        out.append(sc.screen_shot("build-12-join-frame-to-body.png", "printer-status.png"))

        # 13 bezel on
        sc.stage(ALL, {"Bezel": mul(F, 35), "HW Bezel Magnets": mul(F, 35)})
        sc.look(FRONT_RIGHT, target=(0, 10, 45))
        out.append(sc.shot("build-13-bezel.png"))      # no UI: the exploded bezel hides part of the screen

        # 14 top detail: level button caps
        sc.stage(ALL)
        sc.look_at((-21, 38, 80), add(mul(F, 0.9), mul(X, -0.45), mul(Z, 0.9)), 70)
        out.append(sc.screen_shot("build-14-buttons-detail.png", "home.png"))
    finally:
        sc.reset()
        sc.look(FRONT_RIGHT)
    if QUAD_DIR:
        import json
        with open(os.path.join(QUAD_DIR, "screen_quads.json"), "w") as fh:
            json.dump(SCREEN_UI, fh, indent=1)
    return out


def run(context):
    ui = app.userInterface
    try:
        files = main()
        ui.messageBox("Saved %d images to %s" % (len(files), OUT_DIR))
    except:
        ui.messageBox("Failed:\n" + traceback.format_exc())
